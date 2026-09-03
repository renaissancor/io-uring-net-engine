// epoll chat server — single-threaded, level-triggered.
//
// STUDY BUILD. Deliberately uses the STL and a single file. The point is to
// learn the readiness model and to get the *protocol* logic (framing, rooms,
// backpressure, disconnect ordering) correct somewhere cheap to debug, before
// porting to io_uring where completion semantics add their own failure modes.
//
// The lessons this file is built to teach are marked LESSON 1..6 in the code;
// lessons 7 (backpressure on loopback) and 8 (never send() inline while
// walking the room) are in the README, the latter being why CHAT_FLUSH exists.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The tick-budget experiment's logic term and instrument. Every entry point
// is a no-op unless CHAT_TICK_HZ is set, so the loop that produced the rows
// in result-notes/ is unchanged when it is not. See the header.
#include "tick_logic.h"

// ---------------------------------------------------------------- protocol

// 4-byte header, then payload. Little-endian; we assume x86 and do not swap,
// which is exactly the shortcut the real project takes. Its header is 4 bytes
// too — [uint16 size][uint16 id] — and differs only in that `size` counts the
// header while `len` here does not. See README § Protocol.
struct wire_header {
    uint16_t len;   // payload bytes, NOT including this header
    uint16_t type;
};
static_assert(sizeof(wire_header) == 4);

enum : uint16_t {
    c_set_nick = 1,
    c_join     = 2,
    c_chat     = 3,

    s_notice   = 100,
    s_chat     = 101,
    s_tick     = 102,   // stage C: one frame per member per tick, [u16 len][chat payload]...
};

static constexpr size_t k_header_size  = sizeof(wire_header);
static constexpr size_t k_max_payload  = 1024;
static constexpr size_t k_send_cap     = 256 * 1024;   // backpressure limit
// Runtime, not constexpr, because the load generator needs to push past it.
// CHAT_MAX_CONNS raises the cap; RLIMIT_NOFILE must be raised to match, or the
// accept path just trades a polite refusal for the EMFILE livelock (LESSON 6).
static size_t g_max_conns = 4096;

// Above a few thousand connections the per-accept log line is itself the
// bottleneck: it is a syscall per connection on a line-buffered stdout, and it
// makes the server look slow when the measurement is really measuring printf.
static bool g_quiet = false;

// ------------------------------------------------------------------- state

struct conn {
    int         fd = -1;
    std::string nick;
    std::string room;
    std::string in;                  // accumulated recv bytes, may hold a partial frame
    std::string out;                 // pending send bytes
    bool        armed_write = false; // is EPOLLOUT currently in this fd's epoll mask?
    bool        closing     = false; // doomed; skip further work this tick
    bool        dirty       = false; // has unsent bytes queued this batch (batch mode)
};

static int g_ep      = -1;
static int g_reserve = -1;   // see LESSON 6 (EMFILE)

// LESSON 7 — Backpressure is invisible on loopback by default.
// Test affordance. On loopback the kernel auto-tunes SO_SNDBUF to a couple of
// megabytes, so send() happily swallows everything and you can never observe
// EAGAIN, EPOLLOUT arming, or the backpressure path without pushing tens of MB.
// Setting CHAT_SNDBUF=8192 shrinks the kernel's buffer so the userspace `out`
// buffer starts filling almost immediately and the whole path becomes testable.
static int g_sndbuf = 0;     // 0 = leave kernel default alone

// LESSON 8 — Never send() inline while walking the room. This is the switch
// that makes both shapes runnable, and `batch` is the honest one: measure
// io_uring against CHAT_FLUSH=batch, never against immediate. See the README
// for the numbers. Related sites are tagged `LESSON 8` — broadcast(), the
// recv path, flush_dirty(), and the main loop's tail ordering.
//
// Send batching. In `immediate` mode a broadcast calls send() once per
// recipient, inline, while walking the room — one syscall per delivery, and
// the naive shape. In `batch` mode the room walk only appends to each
// recipient's `out`, and one flush pass at the end of the epoll batch sends
// whatever accumulated: several messages bound for the same connection during
// one batch collapse into a single send().
//
// This exists as a switch rather than an edit because the two modes are two
// different baselines, and comparing io_uring against only the naive one would
// credit io_uring for batching that epoll can do perfectly well. A weak
// control group is not a control group.
//
// The latency cost is one epoll batch, not one game tick — microseconds, not
// milliseconds. A fixed-rate tick is a different and much larger change.
static bool g_batch_flush = false;

static std::unordered_map<int, conn>                              g_conns;
static std::unordered_map<std::string, std::unordered_set<int>>   g_rooms;
static std::vector<int>                                           g_doomed;
static std::vector<int>                                           g_dirty;

// Stage C — tick-coalesced delivery (design-notes/2026-09-03-stage-c-*).
// CHAT_TICK_MODE=coalesce: a chat frame is appended to its room's tick buffer
// instead of being broadcast in the batch it arrived in; at the tick every
// member gets the whole buffer as ONE s_tick frame. One send() per player per
// tick instead of F per input. Coalesced, not latest-wins, so every delivery
// still happens exactly once and the client's fan-out gate keeps its meaning.
static bool                                          g_coalesce = false;
static std::unordered_map<std::string, std::string>  g_tick_buf;      // room -> [u16 len][payload]...
static uint64_t                                      g_tick_frames = 0;
static uint64_t                                      g_tick_dropped = 0;
// The client's receive slot is 64 KiB and a u16 length ends at 65,535; a room
// that says more than this in one tick drops the input and counts it, and the
// fan-out gate voids the run, which is the right verdict for that room.
static constexpr size_t k_tick_cap = 32 * 1024;

// ------------------------------------------------------------------ helpers

// Every fd here is created non-blocking at birth (SOCK_NONBLOCK on socket()
// and accept4()), so there is no fcntl(F_SETFL) dance anywhere in this file.

// Recompute the epoll mask for a connection. EPOLLIN is always armed; EPOLLOUT
// is armed only while there are bytes waiting to go out.
//
// LESSON 3 — Never leave EPOLLOUT armed permanently. A writable socket is
// writable almost always, so a permanently-armed EPOLLOUT turns epoll_wait into
// a 100%-CPU spin loop. Arm on partial send, disarm the moment `out` drains.
static void update_epoll_mask(conn& c) {
    const bool want_write = !c.out.empty();
    if (want_write == c.armed_write) return;   // mask unchanged; skip the syscall

    epoll_event ev{};
    ev.events  = EPOLLIN | (want_write ? static_cast<uint32_t>(EPOLLOUT) : 0u);
    ev.data.fd = c.fd;
    if (epoll_ctl(g_ep, EPOLL_CTL_MOD, c.fd, &ev) < 0) {
        std::perror("epoll_ctl MOD");
        return;
    }
    c.armed_write = want_write;
}

// LESSON 5 — Never close() a connection in the middle of iterating over rooms
// or event batches. Mark it doomed and reap after the tick. Closing inline
// invalidates the container you are walking, and worse, the fd number is
// immediately reusable by the next accept() — so a later event in the *same*
// batch can be delivered to a brand-new connection that inherited the number.
static void doom(conn& c) {
    if (c.closing) return;
    c.closing = true;
    g_doomed.push_back(c.fd);
}

static void queue_send(conn& c, uint16_t type, std::string_view payload) {
    if (c.closing) return;
    if (payload.size() > k_max_payload) return;

    // Backpressure: a client that never reads will otherwise grow `out`
    // without bound until the server OOMs. Drop and close — same policy the
    // io_uring design locked in.
    if (c.out.size() + k_header_size + payload.size() > k_send_cap) {
        std::printf("[drop] fd=%d send buffer over cap (%zu B), closing\n",
                    c.fd, c.out.size());
        doom(c);
        return;
    }

    wire_header h{static_cast<uint16_t>(payload.size()), type};
    c.out.append(reinterpret_cast<const char*>(&h), k_header_size);
    c.out.append(payload);
}

// The tick frame is the one payload allowed past k_max_payload; it has its
// own cap (k_tick_cap) and the same backpressure rule.
static void queue_tick_frame(conn& c, std::string_view payload) {
    if (c.closing) return;
    if (c.out.size() + k_header_size + payload.size() > k_send_cap) {
        std::printf("[drop] fd=%d send buffer over cap (%zu B), closing\n",
                    c.fd, c.out.size());
        doom(c);
        return;
    }
    wire_header h{static_cast<uint16_t>(payload.size()), s_tick};
    c.out.append(reinterpret_cast<const char*>(&h), k_header_size);
    c.out.append(payload);
}

// Push as much of `out` as the socket will take.
static void flush_send(conn& c) {
    while (!c.out.empty()) {
        // MSG_NOSIGNAL: without it, writing to a peer that already closed
        // raises SIGPIPE and kills the process. Classic first-server crash.
        ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);

        if (n > 0) {
            c.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;

        // LESSON 2 — EAGAIN on send is not an error. It means the kernel's
        // socket buffer is full. Keep the unsent tail, arm EPOLLOUT, and
        // return; the event loop will call back when there is room again.
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;

        std::printf("[err ] fd=%d send: %s\n", c.fd, std::strerror(errno));
        doom(c);
        return;
    }
    update_epoll_mask(c);
}

static void broadcast(const std::string& room, uint16_t type,
                      std::string_view payload, int except_fd) {
    auto it = g_rooms.find(room);
    if (it == g_rooms.end()) return;

    for (int fd : it->second) {
        if (fd == except_fd) continue;
        auto cit = g_conns.find(fd);
        if (cit == g_conns.end() || cit->second.closing) continue;
        queue_send(cit->second, type, payload);

        // LESSON 8 (and it dissolves LESSON 5b).
        // In batch mode the flush is deferred to the end of the epoll batch.
        // That is not only a syscall optimisation: sending here means a send
        // failure dooms a connection *while this loop is walking the room's
        // member set*, which is the cascade that LESSON 5b is about. Deferring
        // removes the hazard rather than working around it.
        if (!g_batch_flush)
            flush_send(cit->second);
        else if (!cit->second.dirty && !cit->second.out.empty()) {
            cit->second.dirty = true;
            g_dirty.push_back(fd);
        }
    }
}

static void leave_room(conn& c) {
    if (c.room.empty()) return;
    auto it = g_rooms.find(c.room);
    if (it != g_rooms.end()) {
        it->second.erase(c.fd);
        if (it->second.empty()) {
            g_rooms.erase(it);
            if (g_coalesce) g_tick_buf.erase(c.room);   // nobody left to receive it
        }
    }
    c.room.clear();
}

// Stage C, the tick side: every room's buffer goes to every member as one
// frame. Runs inside the timed tick phase (tick::maybe_tick's callback), so
// its cost lands in the tick histogram, where § 7.1 puts "build snapshots";
// the send() calls themselves happen in the flush pass that follows.
static void flush_tick_rooms() {
    for (auto& [room, buf] : g_tick_buf) {
        if (buf.empty()) continue;
        auto it = g_rooms.find(room);
        if (it != g_rooms.end()) {
            for (int fd : it->second) {
                auto cit = g_conns.find(fd);
                if (cit == g_conns.end() || cit->second.closing) continue;
                conn& m = cit->second;
                queue_tick_frame(m, buf);
                ++g_tick_frames;
                if (!g_batch_flush)
                    flush_send(m);
                else if (!m.dirty && !m.out.empty()) {
                    m.dirty = true;
                    g_dirty.push_back(fd);
                }
            }
        }
        buf.clear();
    }
}

// --------------------------------------------------------------- packet work

static void handle_packet(conn& c, uint16_t type, std::string_view payload) {
    switch (type) {
    case c_set_nick: {
        c.nick.assign(payload.substr(0, 31));
        std::string msg = "nick set to " + c.nick;
        queue_send(c, s_notice, msg);
        break;
    }
    case c_join: {
        if (c.nick.empty()) {
            queue_send(c, s_notice, "set a nickname first");
            break;
        }
        std::string joined{payload.substr(0, 63)};
        if (joined.empty()) {
            queue_send(c, s_notice, "room name required");
            break;
        }
        if (!c.room.empty()) {
            std::string bye = c.nick + " left";
            broadcast(c.room, s_notice, bye, c.fd);
            leave_room(c);
        }
        c.room = joined;
        g_rooms[c.room].insert(c.fd);
        queue_send(c, s_notice, "joined " + c.room);

        std::string hello = c.nick + " joined";
        broadcast(c.room, s_notice, hello, c.fd);
        break;
    }
    case c_chat: {
        if (c.room.empty()) {
            queue_send(c, s_notice, "join a room first");
            break;
        }
        tick::on_msg(c.fd);                    // the experiment's c_msg term
        std::string line = c.nick + ": ";
        line.append(payload);
        if (g_coalesce) {
            // Stage C: record, do not send. [u16 len][line] onto the room's
            // tick buffer; flush_tick_rooms() delivers it at the tick.
            std::string& buf = g_tick_buf[c.room];
            if (buf.size() + 2 + line.size() > k_tick_cap) { ++g_tick_dropped; break; }
            const uint16_t elen = static_cast<uint16_t>(line.size());
            buf.append(reinterpret_cast<const char*>(&elen), 2);
            buf.append(line);
            break;
        }
        broadcast(c.room, s_chat, line, -1);   // -1: echo back to sender too
        break;
    }
    default:
        std::printf("[warn] fd=%d unknown type %u, closing\n", c.fd, type);
        doom(c);
        break;
    }
}

// LESSON 4 — TCP is a byte stream, not a message stream. One recv() can return
// half a frame, three frames, or two-and-a-half frames. Loop while a *complete*
// frame is buffered and leave the remainder for the next event.
static void parse_frames(conn& c) {
    while (!c.closing && c.in.size() >= k_header_size) {
        wire_header h{};
        std::memcpy(&h, c.in.data(), k_header_size);

        if (h.len > k_max_payload) {
            std::printf("[warn] fd=%d oversize frame (%u), closing\n", c.fd, h.len);
            doom(c);
            return;
        }
        const size_t frame_size = k_header_size + h.len;
        if (c.in.size() < frame_size) break;   // partial frame — wait for more

        handle_packet(c, h.type, std::string_view(c.in.data() + k_header_size, h.len));

        // O(n) erase. Fine at study scale; the real server uses a ring buffer
        // precisely to avoid this memmove per frame.
        c.in.erase(0, frame_size);
    }
}

static void on_readable(conn& c) {
    char buf[64 * 1024];

    // LESSON 1 — The EAGAIN drain loop. Under LEVEL-triggered epoll this loop
    // is an optimization: if you read once and return, epoll_wait re-reports
    // the fd immediately. Under EDGE-triggered it is MANDATORY — the edge has
    // already fired, and unread bytes will never produce another notification,
    // so the connection silently hangs forever. This is *the* difference
    // between the two modes, and the bug everyone writes once.
    for (;;) {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);

        if (n > 0) {
            if (c.in.size() + static_cast<size_t>(n) > k_max_payload + k_header_size + 65536) {
                std::printf("[warn] fd=%d recv backlog too large, closing\n", c.fd);
                doom(c);
                return;
            }
            c.in.append(buf, static_cast<size_t>(n));
            parse_frames(c);
            if (c.closing) return;
            continue;
        }
        if (n == 0) {                       // orderly shutdown by peer
            doom(c);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // drained

        std::printf("[err ] fd=%d recv: %s\n", c.fd, std::strerror(errno));
        doom(c);
        return;
    }
    if (!g_batch_flush)          // LESSON 8
        flush_send(c);
    else if (!c.dirty && !c.out.empty()) {
        c.dirty = true;
        g_dirty.push_back(c.fd);
    }
}

// LESSON 8 — batch mode's flush pass. Several messages queued to the same connection
// during one epoll batch collapse into a single send() here.
//
// Index loop re-reading size() for the same reason reap_doomed() uses one:
// flush_send() can doom a connection, and while doom() only touches g_doomed
// today, the discipline is cheap and the alternative is a use-after-free the
// day that changes.
static void flush_dirty() {
    for (size_t i = 0; i < g_dirty.size(); ++i) {
        auto it = g_conns.find(g_dirty[i]);
        if (it == g_conns.end()) continue;
        it->second.dirty = false;
        if (it->second.closing) continue;
        flush_send(it->second);
    }
    g_dirty.clear();
}

// ------------------------------------------------------------------- accept

static void on_accept(int listen_fd) {
    for (;;) {
        sockaddr_in addr{};
        socklen_t   alen = sizeof(addr);

        // accept4 with SOCK_NONBLOCK. An accepted socket does NOT inherit
        // O_NONBLOCK from the listener — forgetting this gives you a blocking
        // fd inside a non-blocking event loop, and one slow client stalls
        // every other client on the thread.
        int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // no more pending
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;                  // client vanished pre-accept

            // LESSON 6 — The EMFILE trap. Out of fds means accept4 keeps
            // failing while the listener stays readable, so level-triggered
            // epoll re-reports it forever: a 100%-CPU livelock that never
            // serves anyone. The fix is to hold one fd in reserve, release it
            // to accept the pending connection, close that connection
            // politely, then re-take the reserve.
            if (errno == EMFILE || errno == ENFILE) {
                std::printf("[err ] out of file descriptors; shedding one connection\n");
                if (g_reserve >= 0) {
                    ::close(g_reserve);
                    int victim = ::accept(listen_fd, nullptr, nullptr);
                    if (victim >= 0) ::close(victim);
                    g_reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                }
                break;
            }
            std::perror("accept4");
            break;
        }

        if (g_conns.size() >= g_max_conns) {
            std::printf("[drop] connection cap reached, refusing fd=%d\n", fd);
            ::close(fd);
            continue;
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (g_sndbuf > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &g_sndbuf, sizeof(g_sndbuf));

        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(g_ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
            std::perror("epoll_ctl ADD");
            ::close(fd);
            continue;
        }

        conn c;
        c.fd = fd;
        g_conns.emplace(fd, std::move(c));
        tick::session_add(fd);

        if (!g_quiet) {
            char ip[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            std::printf("[conn] fd=%d from %s:%u (total %zu)\n",
                        fd, ip, ntohs(addr.sin_port), g_conns.size());
        } else if ((g_conns.size() % 1000) == 0) {
            std::printf("[conn] total %zu\n", g_conns.size());
        }
    }
}

// LESSON 5b — Reaping can cascade. The "X left" notice below is a broadcast,
// and a broadcast can hit the send cap on some *other* client, which dooms it,
// which push_back()s onto g_doomed — the very vector we are walking. A
// range-for here is a heap-use-after-free the moment that vector reallocates.
// (ASan caught exactly this on the first slow-reader run.)
//
// Index-loop with a re-read of size() each iteration is both safe and better
// behaved: connections doomed *during* the reap get cleaned up in the same
// pass instead of lingering until the next tick.
static void reap_doomed() {
    for (size_t i = 0; i < g_doomed.size(); ++i) {
        const int fd = g_doomed[i];
        auto it = g_conns.find(fd);
        if (it == g_conns.end()) continue;

        conn& c = it->second;
        if (!c.room.empty() && !c.nick.empty()) {
            const std::string bye  = c.nick + " left";
            const std::string room = c.room;
            leave_room(c);                          // remove first, so the
            broadcast(room, s_notice, bye, fd);     // departing fd can't be sent to
        } else {
            leave_room(c);
        }

        // epoll_ctl DEL is implicit on close(), but doing it explicitly keeps
        // the intent obvious and is required if the fd is duplicated anywhere.
        epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        g_conns.erase(it);
        tick::session_remove(fd);
        std::printf("[disc] fd=%d closed (total %zu)\n", fd, g_conns.size());
    }
    g_doomed.clear();
}

// --------------------------------------------------------------------- boot

static int make_listener(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::perror("socket"); return -1; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }
    return fd;
}

// Signals as an fd. This is the epoll worldview in one function: if it can be
// an fd, it belongs in the event loop, and then there is exactly one blocking
// point in the whole program.
static int make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {   // stop default disposition
        std::perror("sigprocmask");
        return -1;
    }
    int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) std::perror("signalfd");
    return fd;
}

int main(int argc, char** argv) {
    const uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;

    ::signal(SIGPIPE, SIG_IGN);   // belt and braces; we also use MSG_NOSIGNAL

    // Line-buffer stdout so the log is readable live and survives redirection
    // to a file while the process is still running.
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (const char* sb = ::getenv("CHAT_SNDBUF")) {
        g_sndbuf = std::atoi(sb);
        std::printf("[cfg ] SO_SNDBUF forced to %d B on accepted sockets\n", g_sndbuf);
    }

    if (const char* mc = ::getenv("CHAT_MAX_CONNS")) {
        g_max_conns = static_cast<size_t>(std::atoll(mc));
        std::printf("[cfg ] connection cap = %zu\n", g_max_conns);
    }
    if (const char* q = ::getenv("CHAT_QUIET"))
        g_quiet = (std::atoi(q) != 0);

    if (const char* f = ::getenv("CHAT_FLUSH"))
        g_batch_flush = (std::strcmp(f, "batch") == 0);
    std::printf("[cfg ] send flush = %s\n",
                g_batch_flush ? "batch (end of epoll batch)"
                              : "immediate (one send per delivery)");

    if (!tick::init(g_max_conns)) return 1;
    if (const char* m = ::getenv("CHAT_TICK_MODE")) {
        if (std::strcmp(m, "coalesce") == 0) {
            if (!tick::g.enabled) {
                std::fprintf(stderr, "CHAT_TICK_MODE=coalesce needs CHAT_TICK_HZ\n");
                return 1;
            }
            g_coalesce = true;
        } else if (std::strcmp(m, "immediate") != 0) {
            std::fprintf(stderr, "CHAT_TICK_MODE must be immediate or coalesce\n");
            return 1;
        }
    }
    if (tick::g.enabled)
        std::printf("[cfg ] tick delivery = %s\n",
                    g_coalesce ? "coalesce (one frame per member per tick)"
                               : "immediate (broadcast in the arriving batch)");

    // The connection cap is meaningless without the fd budget behind it: the
    // soft RLIMIT_NOFILE defaults to 1024, so a 10k cap without this just
    // reaches LESSON 6's EMFILE path 9k connections early. Raising the soft
    // limit toward the hard limit needs no privilege.
    {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            const rlim_t want = static_cast<rlim_t>(g_max_conns) + 64;
            const rlim_t target = (want > rl.rlim_max) ? rl.rlim_max : want;
            if (rl.rlim_cur < target) {
                rlimit next = rl;
                next.rlim_cur = target;
                if (::setrlimit(RLIMIT_NOFILE, &next) == 0)
                    rl.rlim_cur = target;
                else
                    std::perror("setrlimit");
            }
            std::printf("[cfg ] RLIMIT_NOFILE soft = %llu\n",
                        static_cast<unsigned long long>(rl.rlim_cur));
            if (rl.rlim_cur < want)
                std::printf("[warn] fd limit below cap; EMFILE expected near %llu conns\n",
                            static_cast<unsigned long long>(rl.rlim_cur));
        }
    }

    g_ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (g_ep < 0) { std::perror("epoll_create1"); return 1; }

    const int listen_fd = make_listener(port);
    if (listen_fd < 0) return 1;

    const int sig_fd = make_signalfd();
    if (sig_fd < 0) return 1;

    g_reserve = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(g_ep, EPOLL_CTL_ADD, listen_fd, &ev);
    ev.data.fd = sig_fd;
    epoll_ctl(g_ep, EPOLL_CTL_ADD, sig_fd, &ev);

    std::printf("epoll chat server listening on :%u (level-triggered, single thread)\n", port);

    epoll_event events[256];
    bool running = true;

    while (running) {
        // With a tick the wait is bounded by the next deadline; epoll_pwait2
        // takes the timeout in nanoseconds where epoll_wait rounds to
        // milliseconds, which at a 33 ms period is 3 % of jitter. Without a
        // tick this is the original -1 block through epoll_wait.
        int n;
        const int64_t to = tick::timeout_ns();
        if (to < 0) {
            n = ::epoll_wait(g_ep, events, 256, -1);
        } else {
            timespec ts{static_cast<time_t>(to / 1000000000LL),
                        static_cast<long>(to % 1000000000LL)};
            n = ::epoll_pwait2(g_ep, events, 256, &ts, nullptr);
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        tick::drain_begin();
        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t what = events[i].events;

            if (fd == listen_fd) { on_accept(listen_fd); continue; }

            if (fd == sig_fd) {
                signalfd_siginfo si{};
                while (::read(sig_fd, &si, sizeof(si)) == sizeof(si)) {
                    std::printf("\n[stop] signal %u received, shutting down\n", si.ssi_signo);
                }
                running = false;
                continue;
            }

            auto it = g_conns.find(fd);
            if (it == g_conns.end()) continue;   // reaped earlier in this batch
            conn& c = it->second;
            if (c.closing) continue;

            // Error/hangup first: no point reading or writing a dead socket.
            // Note EPOLLHUP can arrive *with* EPOLLIN and unread data still
            // buffered; we choose to drop it, which is the simple policy.
            if (what & (EPOLLERR | EPOLLHUP)) { doom(c); continue; }

            if (what & EPOLLOUT) flush_send(c);
            if (!c.closing && (what & EPOLLIN)) on_readable(c);
        }
        tick::drain_end();

        // The tick runs after the drain and before the flush, so the sends it
        // would have produced (none yet; the tick only touches its own
        // arena in stage A) leave in the same flush pass as the batch's.
        tick::maybe_tick(g_coalesce ? flush_tick_rooms : nullptr);

        // LESSON 8 — the flush/reap ordering.
        // Order matters. The first flush sends everything this batch queued
        // and may doom connections on send failure; reap_doomed() then cleans
        // those up, and its "X left" broadcasts queue fresh bytes onto the
        // survivors — hence the second flush, without which those notices
        // would sit in `out` with no EPOLLOUT armed and never leave.
        tick::flush_begin();
        if (g_batch_flush) {
            flush_dirty();
            reap_doomed();
            flush_dirty();
        } else {
            reap_doomed();
        }
        tick::flush_end();
    }

    tick::report();
    if (g_coalesce)
        std::printf("[tick] coalesce: %llu tick frames queued, %llu inputs dropped at the %zu B cap\n",
                    static_cast<unsigned long long>(g_tick_frames),
                    static_cast<unsigned long long>(g_tick_dropped), k_tick_cap);

    for (auto& [fd, c] : g_conns) ::close(fd);
    g_conns.clear();
    g_rooms.clear();
    ::close(listen_fd);
    ::close(sig_fd);
    if (g_reserve >= 0) ::close(g_reserve);
    ::close(g_ep);
    std::printf("[stop] clean shutdown\n");
    return 0;
}
