// tests/net/echo_smoke_test.cpp
//
// Phase 0 smoke test for the load-bearing wall: io_uring + busy-poll
// (one-shot SQEs, no submit_and_wait) + mem::packet_pool + lnx::thread
// + lnx::atomic + LNX_CHECK, exercised end-to-end against a real TCP
// client on loopback.
//
// Server runs on an lnx::thread; client runs on the test thread.
// Synchronization between them is a pair of lnx::atomic32 cells
// (bound_port for handshake, stop for shutdown). No mutexes.

#include "memory/packet_pool.h"
#include "runtime/thread.h"
#include "sync/atomic.h"
#include "check.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr usize K_RECV_BUF_BYTES = mem::packet_pool::k_bucket_size_1024;
constexpr u16   K_MAX_CONNS      = 16;
constexpr u32   K_RING_ENTRIES   = 256;
constexpr u16   K_OP_ACCEPT      = 1;
constexpr u16   K_OP_RECV        = 2;
constexpr u16   K_OP_SEND        = 3;

// user_data layout: [ op:16 | idx:16 | reserved:32 ]. Pointer-free so
// CQE dispatch never touches per-conn memory the kernel might still
// own; everything routes through the conn table.
constexpr u64 pack_ud(u16 op, u16 idx) noexcept {
    return (static_cast<u64>(op) << 16) | static_cast<u64>(idx);
}
constexpr u16 unpack_op(u64 ud) noexcept {
    return static_cast<u16>((ud >> 16) & 0xFFFF);
}
constexpr u16 unpack_idx(u64 ud) noexcept {
    return static_cast<u16>(ud & 0xFFFF);
}

struct conn {
    int   fd        = -1;
    byte* recv_buf  = nullptr;
    u32   send_off  = 0;
    u32   send_len  = 0;
    bool  in_use    = false;
};

struct server_state {
    io_uring        ring{};
    int             listen_fd       = -1;
    sockaddr_in     accept_addr{};
    socklen_t       accept_addr_len = 0;
    conn            conns[K_MAX_CONNS]{};
    lnx::atomic32&  stop;
    lnx::atomic32&  bound_port;

    server_state(lnx::atomic32& s, lnx::atomic32& p) noexcept
        : stop(s), bound_port(p) {}
};

u16 alloc_conn(server_state& s) noexcept {
    for (u16 i = 0; i < K_MAX_CONNS; ++i) {
        if (!s.conns[i].in_use) {
            s.conns[i].in_use = true;
            return i;
        }
    }
    LNX_CHECK(false);  // K_MAX_CONNS exceeded — smoke test only opens one
    return 0;
}

void release_conn(server_state& s, u16 idx) noexcept {
    conn& c = s.conns[idx];
    if (c.recv_buf != nullptr) {
        mem::packet_pool::instance().release(c.recv_buf, K_RECV_BUF_BYTES);
        c.recv_buf = nullptr;
    }
    if (c.fd >= 0) {
        ::close(c.fd);
        c.fd = -1;
    }
    c.send_off = 0;
    c.send_len = 0;
    c.in_use   = false;
}

void submit_accept(server_state& s) noexcept {
    io_uring_sqe* sqe = io_uring_get_sqe(&s.ring);
    LNX_CHECK(sqe != nullptr);
    s.accept_addr_len = sizeof(s.accept_addr);
    io_uring_prep_accept(sqe, s.listen_fd,
                         reinterpret_cast<sockaddr*>(&s.accept_addr),
                         &s.accept_addr_len, 0);
    io_uring_sqe_set_data64(sqe, pack_ud(K_OP_ACCEPT, 0));
}

void submit_recv(server_state& s, u16 idx) noexcept {
    conn& c = s.conns[idx];
    io_uring_sqe* sqe = io_uring_get_sqe(&s.ring);
    LNX_CHECK(sqe != nullptr);
    io_uring_prep_recv(sqe, c.fd, c.recv_buf, K_RECV_BUF_BYTES, 0);
    io_uring_sqe_set_data64(sqe, pack_ud(K_OP_RECV, idx));
}

void submit_send(server_state& s, u16 idx) noexcept {
    conn& c = s.conns[idx];
    io_uring_sqe* sqe = io_uring_get_sqe(&s.ring);
    LNX_CHECK(sqe != nullptr);
    io_uring_prep_send(sqe, c.fd,
                       c.recv_buf + c.send_off,
                       c.send_len - c.send_off, 0);
    io_uring_sqe_set_data64(sqe, pack_ud(K_OP_SEND, idx));
}

void* server_main(void* arg) noexcept {
    auto& s = *static_cast<server_state*>(arg);

    mem::packet_pool::instance().prewarm();

    s.listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    LNX_CHECK(s.listen_fd >= 0);

    int yes = 1;
    LNX_CHECK(::setsockopt(s.listen_fd, SOL_SOCKET, SO_REUSEADDR,
                           &yes, sizeof(yes)) == 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port        = htons(0);
    LNX_CHECK(::bind(s.listen_fd,
                     reinterpret_cast<sockaddr*>(&bind_addr),
                     sizeof(bind_addr)) == 0);

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    LNX_CHECK(::getsockname(s.listen_fd,
                            reinterpret_cast<sockaddr*>(&bound),
                            &bound_len) == 0);
    LNX_CHECK(::listen(s.listen_fd, 8) == 0);

    LNX_CHECK(io_uring_queue_init(K_RING_ENTRIES, &s.ring, 0) == 0);

    submit_accept(s);

    // Publish the port AFTER the accept SQE is queued, so a client that
    // races us cannot connect to an unarmed listener. release-store
    // pairs with the test thread's acquire-load.
    s.bound_port.store_release(static_cast<i32>(ntohs(bound.sin_port)));

    while (s.stop.load_acquire() == 0) {
        int submitted = io_uring_submit(&s.ring);
        LNX_CHECK(submitted >= 0);

        io_uring_cqe* cqe = nullptr;
        while (io_uring_peek_cqe(&s.ring, &cqe) == 0) {
            const u64 ud  = io_uring_cqe_get_data64(cqe);
            const u16 op  = unpack_op(ud);
            const u16 idx = unpack_idx(ud);
            const i32 res = cqe->res;
            io_uring_cqe_seen(&s.ring, cqe);

            switch (op) {
            case K_OP_ACCEPT: {
                if (res < 0) {
                    submit_accept(s);
                    break;
                }
                u16 new_idx = alloc_conn(s);
                conn& c = s.conns[new_idx];
                c.fd = res;
                c.recv_buf = static_cast<byte*>(
                    mem::packet_pool::instance().acquire(K_RECV_BUF_BYTES));
                submit_recv(s, new_idx);
                submit_accept(s);
                break;
            }
            case K_OP_RECV: {
                if (res <= 0) {
                    release_conn(s, idx);
                    break;
                }
                conn& c = s.conns[idx];
                c.send_off = 0;
                c.send_len = static_cast<u32>(res);
                submit_send(s, idx);
                break;
            }
            case K_OP_SEND: {
                if (res < 0) {
                    release_conn(s, idx);
                    break;
                }
                conn& c = s.conns[idx];
                c.send_off += static_cast<u32>(res);
                if (c.send_off < c.send_len) {
                    submit_send(s, idx);
                } else {
                    submit_recv(s, idx);
                }
                break;
            }
            default:
                LNX_CHECK(false);
            }
        }
    }

    for (u16 i = 0; i < K_MAX_CONNS; ++i) {
        if (s.conns[i].in_use) {
            release_conn(s, i);
        }
    }
    if (s.listen_fd >= 0) {
        ::close(s.listen_fd);
        s.listen_fd = -1;
    }
    io_uring_queue_exit(&s.ring);
    return nullptr;
}

int connect_loopback(i32 port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    LNX_CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u16>(port));
    LNX_CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)) == 0);
    return fd;
}

void wake_pending_accept(i32 port) noexcept {
    // Server's busy-poll exits on stop check, but a queued accept SQE
    // is still parked in the kernel. A throwaway connect-then-close
    // satisfies it so the io_uring_queue_exit path is reached.
    //
    // Deliberately NOT connect_loopback(): this connect races the server
    // observing `stop` and closing the listener, and losing that race is
    // fine — a refused connect means the server is already past the parked
    // accept. Checking `== 0` here asserts the absence of a benign race
    // (flaked as SIGTRAP on 2-vCPU CI runners under full-suite load).
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    LNX_CHECK(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u16>(port));
    (void)::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
}

}  // namespace

TEST_CASE("net: io_uring echo server round-trips 1000 messages",
          "[net][smoke][io_uring]") {
    lnx::atomic32 stop{0};
    lnx::atomic32 bound_port{0};
    server_state state{stop, bound_port};

    lnx::thread srv{server_main, &state};

    while (bound_port.load_acquire() == 0) {
        lnx::this_thread::yield();
    }
    const i32 port = bound_port.load_acquire();
    REQUIRE(port > 0);

    int cfd = connect_loopback(port);

    constexpr int  K_N_MESSAGES = 1000;
    constexpr usize K_MSG_CAP   = 64;
    char tx[K_MSG_CAP];
    char rx[K_MSG_CAP];

    for (int i = 0; i < K_N_MESSAGES; ++i) {
        const int written = std::snprintf(tx, sizeof(tx), "ping-%d", i);
        REQUIRE(written > 0);
        const usize msg_len = static_cast<usize>(written);

        ssize_t sent = ::send(cfd, tx, msg_len, 0);
        REQUIRE(sent == static_cast<ssize_t>(msg_len));

        usize received = 0;
        while (received < msg_len) {
            ssize_t got = ::recv(cfd, rx + received,
                                 msg_len - received, 0);
            REQUIRE(got > 0);
            received += static_cast<usize>(got);
        }
        REQUIRE(std::memcmp(tx, rx, msg_len) == 0);
    }

    ::close(cfd);
    stop.store_release(1);
    wake_pending_accept(port);
    srv.join();

    SUCCEED();
}
