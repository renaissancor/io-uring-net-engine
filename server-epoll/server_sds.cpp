// epoll chat server on the engine's primitives — single-threaded, level-triggered,
// no STL. The second build of the control group.
//
// This file is server.cpp with its containers swapped and nothing else moved:
// same protocol, same eight lessons, same env knobs, same 256 KiB send cap and
// the same "[drop] ... over cap" line that the saturation note counts. It
// exists so that the STL-versus-sds delta is a measured row, and so that the
// epoll control and the io_uring server share their data structures and
// differ only in the I/O mechanism. Read server.cpp first; the LESSON n
// markers here point at the same places and are not re-explained.
//
// What is different, and why (result-notes/2026-09-0X and the design note
// 2026-09-0X-control-group-on-engine-primitives.md carry the numbers):
//   - the connection table is a flat array indexed by fd inside one
//     mmap(MAP_NORESERVE) slab; a slot is constructed on accept and destroyed
//     on close, so the only per-connection cost is the pages it touches;
//   - inbound bytes live in an sds::ring_buffer, outbound bytes in a linear
//     queue that resets when drained — no erase(0, n), no growth, no malloc;
//   - rooms are records in the slab found through sds::cstr_hash_map, and
//     membership is an intrusive doubly-linked list through the connections;
//   - the doomed and dirty lists are sds::malloc_vector reserved once at boot;
//   - the recv() call shape is deliberately the SAME as server.cpp's (64 KiB
//     scratch, loop to EAGAIN), so the delta is data structures and not
//     syscalls. CHAT_SHORT_READ=1 opts into the short-read stop as its own row.
//
// The engine headers come from an INSTALL PREFIX through find_package
// (CMakeLists.txt next to this file), never from ../engine-uring/src — the
// same seam server-uring uses. If a header is missing here, the fix is the
// engine's public FILE_SET, not a relative include.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/mman.h>
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
#include <new>
#include <string_view>

#include <check.h>
#include <sds/cstr_hash_map.h>
#include <sds/malloc_vector.h>
#include <sds/ring_buffer.h>
#include <types.h>

// ---------------------------------------------------------------- protocol

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
};

static constexpr usize k_header_size = sizeof(wire_header);
static constexpr usize k_max_payload = 1024;
static constexpr usize k_max_frame   = k_header_size + k_max_payload;
static constexpr usize k_send_cap    = 256 * 1024;   // backpressure limit, as server.cpp
static constexpr usize k_nick_max    = 31;
static constexpr usize k_room_max    = 63;

// ------------------------------------------------------------------- sizing
//
// Inbound ring. Bounded and power-of-two, as sds::ring_buffer requires. It
// has to hold one maximum frame plus whatever a recv() delivers before
// parse_frames() runs; parse consumes every complete frame, so what carries
// over is at most one partial frame (< k_max_frame). recv() asks for
// min(64 KiB, free), so the one place the syscall shape differs from
// server.cpp is a connection with more than ~31 KiB unread, which under the
// measured loads (≈6.6 KB per connection at the 1024 B ceiling) does not
// occur. Its pages all get touched over a connection's life: ~32 KiB resident
// per connection, released on close.
static constexpr usize k_rx_ring = 32 * 1024;

// Outbound queue. A ring of k_send_cap per connection would touch every one
// of its pages as the cursors cycle — 2.5 GB resident at 10k connections —
// so this is a LINEAR queue [tx_head, tx_tail) that resets to 0 when it
// drains, which in batch mode is after nearly every flush. Only the peak
// queue depth is ever resident. The region is the cap plus one frame so the
// cap check is the same arithmetic as server.cpp:162: over cap means doom,
// under cap always fits (after a compaction, see queue_send).
static constexpr usize k_tx_region = k_send_cap + k_max_frame;

static usize g_max_conns = 4096;     // CHAT_MAX_CONNS, as server.cpp
static bool  g_quiet       = false;  // CHAT_QUIET
static int   g_sndbuf      = 0;      // CHAT_SNDBUF (LESSON 7)
static bool  g_batch_flush = false;  // CHAT_FLUSH=batch (LESSON 8)
static bool  g_short_read  = false;  // CHAT_SHORT_READ=1: stop the drain at a short read

// ------------------------------------------------------------------- state

static constexpr u32 k_no_room = ~0u;

struct conn {
    int  fd          = -1;
    bool armed_write = false;   // is EPOLLOUT in this fd's epoll mask? (LESSON 3)
    bool closing     = false;   // doomed; skip further work this tick (LESSON 5)
    bool dirty       = false;   // on g_dirty this batch (LESSON 8)
    u08  nick_len    = 0;
    u32  room        = k_no_room;   // index into the room slab
    int  room_prev   = -1;          // intrusive membership list, by fd
    int  room_next   = -1;
    char nick[k_nick_max + 1] = {};

    sds::ring_buffer<k_rx_ring, sds::ring_sync::single> in;

    usize tx_head = 0;
    usize tx_tail = 0;
    byte  tx[k_tx_region];      // deliberately NOT initialised: touching it
                                // would fault in 257 KiB per accept

    explicit conn(int f) noexcept : fd(f) {}
    conn(const conn&)            = delete;
    conn& operator=(const conn&) = delete;
};

struct room {
    char name[k_room_max + 1];
    int  head;        // first member fd, -1 when empty
    u32  count;
    u32  next_free;   // free-list link while unused
    bool live;
};

// The slab. One anonymous MAP_NORESERVE mapping holding g_slots connection
// slots at a page-multiple stride, indexed by fd. Pages are only faulted in
// when a slot is constructed and written, and MADV_DONTNEED on close hands
// them back, so RSS follows live connections and not the cap.
static usize  g_slots      = 0;
static usize  g_slot_bytes = 0;
static byte*  g_slab       = nullptr;
static u08*   g_live       = nullptr;    // 1 when the slot holds a constructed conn
static room*  g_rooms      = nullptr;    // g_slots records; each conn is in <= 1 room
static u32    g_room_free  = k_no_room;  // free-list head
static sds::cstr_hash_map<u32> g_room_index(1024);   // name -> room slot; key borrowed
                                                     // from room::name (stable)
static sds::malloc_vector<int> g_doomed;
static sds::malloc_vector<int> g_dirty;

static int g_ep      = -1;
static int g_reserve = -1;   // LESSON 6 (EMFILE)

static inline conn& slot(int fd) noexcept {
    return *reinterpret_cast<conn*>(g_slab + static_cast<usize>(fd) * g_slot_bytes);
}
static inline bool live(int fd) noexcept {
    return fd >= 0 && static_cast<usize>(fd) < g_slots && g_live[fd];
}

// malloc_vector::push_back drops silently when it cannot grow (soft-OOM
// contract). Both lists are reserved to the slot count at boot so that can
// only mean a bug; a doomed fd that is not recorded is a leaked fd.
static inline void push_checked(sds::malloc_vector<int>& v, int fd) noexcept {
    const usize before = v.size();
    v.push_back(fd);
    LNX_CHECK(v.size() == before + 1);
}

// ------------------------------------------------------------------ helpers

// LESSON 3 — arm EPOLLOUT only while bytes are waiting.
static void update_epoll_mask(conn& c) {
    const bool want_write = c.tx_head < c.tx_tail;
    if (want_write == c.armed_write) return;

    epoll_event ev{};
    ev.events  = EPOLLIN | (want_write ? static_cast<uint32_t>(EPOLLOUT) : 0u);
    ev.data.fd = c.fd;
    if (epoll_ctl(g_ep, EPOLL_CTL_MOD, c.fd, &ev) < 0) {
        std::perror("epoll_ctl MOD");
        return;
    }
    c.armed_write = want_write;
}

// LESSON 5 — mark doomed, reap after the tick.
static void doom(conn& c) {
    if (c.closing) return;
    c.closing = true;
    push_checked(g_doomed, c.fd);
}

static void queue_send(conn& c, uint16_t type, const void* payload, usize len) {
    if (c.closing) return;
    if (len > k_max_payload) return;

    const usize used  = c.tx_tail - c.tx_head;
    const usize frame = k_header_size + len;

    // Backpressure: same policy, same threshold, same line as server.cpp —
    // the saturation note counts this line to find the ceiling.
    if (used + frame > k_send_cap) {
        std::printf("[drop] fd=%d send buffer over cap (%zu B), closing\n", c.fd, used);
        doom(c);
        return;
    }

    // Under the cap the frame always fits in the region, but possibly not at
    // the current tail: a connection that is repeatedly part-sent (EPOLLOUT
    // armed, never fully drained) walks its tail toward the region end.
    // Compact to offset 0 then — bounded by the cap and rare, where an
    // std::string::erase(0, n) paid a memmove on every send.
    if (c.tx_tail + frame > k_tx_region) {
        std::memmove(c.tx, c.tx + c.tx_head, used);
        c.tx_head = 0;
        c.tx_tail = used;
    }

    const wire_header h{static_cast<uint16_t>(len), type};
    std::memcpy(c.tx + c.tx_tail, &h, k_header_size);
    if (len) std::memcpy(c.tx + c.tx_tail + k_header_size, payload, len);
    c.tx_tail += frame;
}

static inline void queue_send(conn& c, uint16_t type, std::string_view s) {
    queue_send(c, type, s.data(), s.size());
}

// Push as much of the queue as the socket will take.
static void flush_send(conn& c) {
    while (c.tx_head < c.tx_tail) {
        const ssize_t n = ::send(c.fd, c.tx + c.tx_head, c.tx_tail - c.tx_head, MSG_NOSIGNAL);

        if (n > 0) {
            c.tx_head += static_cast<usize>(n);
            if (c.tx_head == c.tx_tail) c.tx_head = c.tx_tail = 0;   // drained: reset
            continue;
        }
        if (n < 0 && errno == EINTR) continue;

        // LESSON 2 — EAGAIN on send is not an error.
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;

        std::printf("[err ] fd=%d send: %s\n", c.fd, std::strerror(errno));
        doom(c);
        return;
    }
    update_epoll_mask(c);
}

static void mark_dirty(conn& c) {
    if (!c.dirty && c.tx_head < c.tx_tail) {
        c.dirty = true;
        push_checked(g_dirty, c.fd);
    }
}

static void broadcast(u32 room_idx, uint16_t type, const void* payload, usize len,
                      int except_fd) {
    if (room_idx == k_no_room || !g_rooms[room_idx].live) return;

    // The membership list is walked by fd; queue_send never unlinks, so the
    // next pointer is stable across the call. (Only leave_room unlinks, and
    // it is never called from inside a broadcast — LESSON 5b.)
    for (int fd = g_rooms[room_idx].head; fd != -1; fd = slot(fd).room_next) {
        if (fd == except_fd) continue;
        conn& r = slot(fd);
        if (r.closing) continue;
        queue_send(r, type, payload, len);

        // LESSON 8 (and it dissolves LESSON 5b).
        if (!g_batch_flush) flush_send(r);
        else                mark_dirty(r);
    }
}

// ------------------------------------------------------------------- rooms

static u32 room_acquire(std::string_view name) {
    if (auto it = g_room_index.find(name.data()); it != g_room_index.end())
        return it->second;

    LNX_CHECK(g_room_free != k_no_room);   // slots == conns, each conn in <= 1 room
    const u32 idx = g_room_free;
    room& r = g_rooms[idx];
    g_room_free = r.next_free;

    std::memcpy(r.name, name.data(), name.size());
    r.name[name.size()] = '\0';
    r.head  = -1;
    r.count = 0;
    r.live  = true;
    // The map borrows r.name; the record outlives the key because the key is
    // erased in room_release() BEFORE the slot returns to the free list.
    g_room_index.insert(r.name, idx);
    return idx;
}

static void room_release(u32 idx) {
    room& r = g_rooms[idx];
    g_room_index.erase(r.name);     // before the slot is reusable
    r.live      = false;
    r.head      = -1;
    r.next_free = g_room_free;
    g_room_free = idx;
}

static void join_room(conn& c, u32 idx) {
    room& r = g_rooms[idx];
    c.room      = idx;
    c.room_prev = -1;
    c.room_next = r.head;
    if (r.head != -1) slot(r.head).room_prev = c.fd;
    r.head = c.fd;
    ++r.count;
}

static void leave_room(conn& c) {
    if (c.room == k_no_room) return;
    room& r = g_rooms[c.room];
    if (c.room_prev != -1) slot(c.room_prev).room_next = c.room_next;
    else                   r.head = c.room_next;
    if (c.room_next != -1) slot(c.room_next).room_prev = c.room_prev;
    c.room_prev = c.room_next = -1;
    if (--r.count == 0) room_release(c.room);
    c.room = k_no_room;
}

// A room name has to be NUL-terminated for the borrowed-key map; the frame
// payload is not. One bounded scratch copy per join, off the hot path.
static std::string_view room_name_scratch(std::string_view payload) {
    static char buf[k_room_max + 1];
    const usize n = payload.size() < k_room_max ? payload.size() : k_room_max;
    std::memcpy(buf, payload.data(), n);
    buf[n] = '\0';
    return std::string_view(buf, n);
}

// --------------------------------------------------------------- packet work

// "nick: payload" and the notices are assembled once per inbound message in
// this scratch and copied once per recipient by queue_send — where server.cpp
// built a std::string (a malloc/free pair per message) and then copied it.
static byte g_line[k_nick_max + 2 + k_max_payload];

static void handle_packet(conn& c, uint16_t type, std::string_view payload) {
    switch (type) {
    case c_set_nick: {
        const usize n = payload.size() < k_nick_max ? payload.size() : k_nick_max;
        std::memcpy(c.nick, payload.data(), n);
        c.nick[n]  = '\0';
        c.nick_len = static_cast<u08>(n);
        constexpr std::string_view pre = "nick set to ";
        std::memcpy(g_line, pre.data(), pre.size());
        std::memcpy(g_line + pre.size(), c.nick, n);
        queue_send(c, s_notice, g_line, pre.size() + n);
        break;
    }
    case c_join: {
        if (c.nick_len == 0) {
            queue_send(c, s_notice, "set a nickname first");
            break;
        }
        const std::string_view joined = room_name_scratch(payload);
        if (joined.empty()) {
            queue_send(c, s_notice, "room name required");
            break;
        }
        if (c.room != k_no_room) {
            std::memcpy(g_line, c.nick, c.nick_len);
            std::memcpy(g_line + c.nick_len, " left", 5);
            broadcast(c.room, s_notice, g_line, c.nick_len + 5, c.fd);
            leave_room(c);
        }
        join_room(c, room_acquire(joined));

        constexpr std::string_view pre = "joined ";
        std::memcpy(g_line, pre.data(), pre.size());
        std::memcpy(g_line + pre.size(), g_rooms[c.room].name, joined.size());
        queue_send(c, s_notice, g_line, pre.size() + joined.size());

        std::memcpy(g_line, c.nick, c.nick_len);
        std::memcpy(g_line + c.nick_len, " joined", 7);
        broadcast(c.room, s_notice, g_line, c.nick_len + 7, c.fd);
        break;
    }
    case c_chat: {
        if (c.room == k_no_room) {
            queue_send(c, s_notice, "join a room first");
            break;
        }
        std::memcpy(g_line, c.nick, c.nick_len);
        std::memcpy(g_line + c.nick_len, ": ", 2);
        std::memcpy(g_line + c.nick_len + 2, payload.data(), payload.size());
        broadcast(c.room, s_chat, g_line, c.nick_len + 2 + payload.size(), -1);   // echo too
        break;
    }
    default:
        std::printf("[warn] fd=%d unknown type %u, closing\n", c.fd, type);
        doom(c);
        break;
    }
}

// LESSON 4 — TCP is a byte stream. Consume complete frames off the ring; a
// partial frame stays queued. The header is peeked (wrap-safe, no advance);
// a frame that sits contiguously is handled in place through the ring's
// direct pointer and committed afterwards, one that straddles the wrap is
// dequeued into a scratch first. Nothing is erased or moved.
static void parse_frames(conn& c) {
    static byte scratch[k_max_frame];

    while (!c.closing && c.in.used_size() >= k_header_size) {
        wire_header h{};
        c.in.peek(reinterpret_cast<byte*>(&h), k_header_size);

        if (h.len > k_max_payload) {
            std::printf("[warn] fd=%d oversize frame (%u), closing\n", c.fd, h.len);
            doom(c);
            return;
        }
        const usize frame = k_header_size + h.len;
        if (c.in.used_size() < frame) break;   // partial frame — wait for more

        if (c.in.direct_dequeue_size() >= frame) {
            const byte* p = c.in.direct_dequeue_ptr();
            handle_packet(c, h.type,
                          std::string_view(reinterpret_cast<const char*>(p) + k_header_size, h.len));
            c.in.commit_dequeue(frame);
        } else {
            c.in.dequeue(scratch, frame);
            handle_packet(c, h.type,
                          std::string_view(reinterpret_cast<const char*>(scratch) + k_header_size, h.len));
        }
    }
}

static void on_readable(conn& c) {
    // Same shape as server.cpp: a 64 KiB stack scratch, appended into the
    // connection's buffer — one copy there, one copy here. Filling the ring
    // in place through direct_enqueue_ptr() is the io_uring path's shape and
    // is deliberately NOT used: its contiguous run shrinks near the wrap and
    // would turn one recv() into several, which would put a syscall
    // difference inside a data-structure comparison.
    static byte buf[64 * 1024];

    // LESSON 1 — the EAGAIN drain loop (an optimisation under level-triggered
    // epoll; CHAT_SHORT_READ=1 measures the other choice).
    for (;;) {
        const usize want = c.in.free_size() < sizeof(buf) ? c.in.free_size() : sizeof(buf);
        if (want == 0) {
            std::printf("[warn] fd=%d recv backlog too large, closing\n", c.fd);
            doom(c);
            return;
        }
        const ssize_t n = ::recv(c.fd, buf, want, 0);

        if (n > 0) {
            LNX_CHECK(c.in.enqueue(buf, static_cast<usize>(n)) == static_cast<usize>(n));
            parse_frames(c);
            if (c.closing) return;
            if (g_short_read && static_cast<usize>(n) < want) break;   // drained
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
    if (!g_batch_flush) flush_send(c);          // LESSON 8
    else                mark_dirty(c);
}

// LESSON 8 — batch mode's flush pass. Index loop re-reading size(): flush_send
// can doom, and doom pushes onto g_doomed, not g_dirty — but the discipline is
// what keeps that safe the day it changes.
static void flush_dirty() {
    for (usize i = 0; i < g_dirty.size(); ++i) {
        const int fd = g_dirty[i];
        if (!live(fd)) continue;
        conn& c = slot(fd);
        c.dirty = false;
        if (c.closing) continue;
        flush_send(c);
    }
    g_dirty.clear();
}

// ------------------------------------------------------------------- accept

static usize g_total_live = 0;   // constructed slots; the accept side adds, the reap side subtracts

static void on_accept(int listen_fd) {
    for (;;) {
        sockaddr_in addr{};
        socklen_t   alen = sizeof(addr);

        int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &alen,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;

            // LESSON 6 — the EMFILE trap.
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

        // The cap, plus the one condition the flat table adds: an fd past the
        // last slot. fds are handed out lowest-free, so with at most
        // g_max_conns connections open this only fires if something else in
        // the process holds a great many descriptors.
        if (g_total_live >= g_max_conns || static_cast<usize>(fd) >= g_slots) {
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

        LNX_CHECK(!g_live[fd]);
        ::new (&slot(fd)) conn(fd);   // constructs the ring cursors and scalars only
        g_live[fd] = 1;
        ++g_total_live;

        if (!g_quiet) {
            char ip[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
            std::printf("[conn] fd=%d from %s:%u (total %zu)\n",
                        fd, ip, ntohs(addr.sin_port), g_total_live);
        } else if ((g_total_live % 1000) == 0) {
            std::printf("[conn] total %zu\n", g_total_live);
        }
    }
}

// LESSON 5b — reaping can cascade; index loop re-reading size().
static void reap_doomed() {
    for (usize i = 0; i < g_doomed.size(); ++i) {
        const int fd = g_doomed[i];
        if (!live(fd)) continue;
        conn& c = slot(fd);

        if (c.room != k_no_room && c.nick_len) {
            const u32 room_idx = c.room;
            std::memcpy(g_line, c.nick, c.nick_len);
            std::memcpy(g_line + c.nick_len, " left", 5);
            leave_room(c);                                              // remove first, so the
            broadcast(room_idx, s_notice, g_line, c.nick_len + 5, fd);  // departing fd can't be sent to
        } else {
            leave_room(c);
        }

        epoll_ctl(g_ep, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        c.~conn();
        g_live[fd] = 0;
        // Hand the slot's pages back: a shedding rung fills send queues toward
        // the cap right before it closes them, and without this the RSS of a
        // collapsed run would stay behind.
        ::madvise(&slot(fd), g_slot_bytes, MADV_DONTNEED);
        --g_total_live;
        std::printf("[disc] fd=%d closed (total %zu)\n", fd, g_total_live);
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

static int make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        std::perror("sigprocmask");
        return -1;
    }
    int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0) std::perror("signalfd");
    return fd;
}

// The slab: g_slots = cap + 64 (listener, signalfd, reserve, stdio, slack —
// the same 64 the RLIMIT_NOFILE request adds). Slot stride is sizeof(conn)
// rounded up to a page so MADV_DONTNEED can address one slot exactly.
static bool make_slab() {
    const long page = ::sysconf(_SC_PAGESIZE);
    g_slots      = g_max_conns + 64;
    g_slot_bytes = (sizeof(conn) + static_cast<usize>(page) - 1) & ~(static_cast<usize>(page) - 1);
    const usize bytes = g_slots * g_slot_bytes;

    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap slab");
        return false;
    }
    g_slab = static_cast<byte*>(p);

    g_live  = static_cast<u08*>(std::calloc(g_slots, 1));
    g_rooms = static_cast<room*>(std::calloc(g_slots, sizeof(room)));
    if (!g_live || !g_rooms) {
        std::perror("calloc");
        return false;
    }
    for (usize i = 0; i < g_slots; ++i) {   // thread the room free list
        g_rooms[i].next_free = (i + 1 < g_slots) ? static_cast<u32>(i + 1) : k_no_room;
        g_rooms[i].head      = -1;
    }
    g_room_free = 0;

    g_doomed.reserve(g_slots);
    g_dirty.reserve(g_slots);
    LNX_CHECK(g_doomed.capacity() >= g_slots && g_dirty.capacity() >= g_slots);

    std::printf("[cfg ] slab: %zu slots x %zu KiB = %zu MiB virtual (MAP_NORESERVE; "
                "a slot is resident only while its connection lives)\n",
                g_slots, g_slot_bytes / 1024, bytes / (1024 * 1024));
    return true;
}

int main(int argc, char** argv) {
    const uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;

    ::signal(SIGPIPE, SIG_IGN);
    ::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (const char* sb = ::getenv("CHAT_SNDBUF")) {
        g_sndbuf = std::atoi(sb);
        std::printf("[cfg ] SO_SNDBUF forced to %d B on accepted sockets\n", g_sndbuf);
    }
    if (const char* mc = ::getenv("CHAT_MAX_CONNS")) {
        g_max_conns = static_cast<usize>(std::atoll(mc));
        std::printf("[cfg ] connection cap = %zu\n", g_max_conns);
    }
    if (const char* q = ::getenv("CHAT_QUIET"))
        g_quiet = (std::atoi(q) != 0);
    if (const char* f = ::getenv("CHAT_FLUSH"))
        g_batch_flush = (std::strcmp(f, "batch") == 0);
    std::printf("[cfg ] send flush = %s\n",
                g_batch_flush ? "batch (end of epoll batch)"
                              : "immediate (one send per delivery)");
    if (const char* sr = ::getenv("CHAT_SHORT_READ"))
        g_short_read = (std::atoi(sr) != 0);
    std::printf("[cfg ] recv drain = %s\n",
                g_short_read ? "stop at short read (CHAT_SHORT_READ=1)"
                             : "loop to EAGAIN (as server.cpp)");

    // RLIMIT_NOFILE, as server.cpp.
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

    if (!make_slab()) return 1;

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

    std::printf("epoll chat server (sds) listening on :%u (level-triggered, single thread, no STL)\n",
                port);

    epoll_event events[256];
    bool running = true;

    while (running) {
        int n = ::epoll_wait(g_ep, events, 256, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

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

            if (!live(fd)) continue;   // reaped earlier in this batch
            conn& c = slot(fd);
            if (c.closing) continue;

            if (what & (EPOLLERR | EPOLLHUP)) { doom(c); continue; }

            if (what & EPOLLOUT) flush_send(c);
            if (!c.closing && (what & EPOLLIN)) on_readable(c);
        }

        // LESSON 8 — the flush/reap ordering, as server.cpp.
        if (g_batch_flush) {
            flush_dirty();
            reap_doomed();
            flush_dirty();
        } else {
            reap_doomed();
        }
    }

    for (usize fd = 0; fd < g_slots; ++fd)
        if (g_live[fd]) { ::close(static_cast<int>(fd)); slot(static_cast<int>(fd)).~conn(); g_live[fd] = 0; }
    ::munmap(g_slab, g_slots * g_slot_bytes);
    std::free(g_live);
    std::free(g_rooms);
    ::close(listen_fd);
    ::close(sig_fd);
    if (g_reserve >= 0) ::close(g_reserve);
    ::close(g_ep);
    std::printf("[stop] clean shutdown\n");
    return 0;
}
