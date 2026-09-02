// conn.h — per-connection state and the process-wide clock and stop flag.
//
// Shared by both phases so they cannot drift into two definitions of what a
// connection is. Nothing subtle lives here; if something subtle appears, it
// probably belongs in the phase that needs it.
#pragma once

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

// ------------------------------------------------------------------- state

enum class conn_state : uint8_t { none, connecting, ready, dead };

struct conn {
    conn_state  state = conn_state::none;
    int         index = -1;   // logical client id, for nick/room
    uint32_t    seq   = 0;
    std::string pending;      // unsent tail; empty except after a partial send
    // Receive buffer: a fixed k_rx_cap slot in one mmap'd slab, indexed by fd
    // (netutil.cpp rx_slab_*). recv() lands directly in it, frames are parsed
    // in place, and only a partial-frame remainder is ever moved. There is no
    // per-message copy or allocation on the receive path, and no 64 KiB
    // scratch: the slot IS the scratch. rx is resolved lazily so that the
    // `c = conn{}` resets in connect.cpp stay valid.
    char*       rx     = nullptr;
    uint32_t    rx_len = 0;   // bytes buffered, from rx[0]; may end in a partial frame
};

// One recv() asks for up to this many bytes, exactly as the retired 64 KiB
// stack scratch did, so the recv sizing is identical under any --rcvbuf.
constexpr size_t k_rx_cap = 64 * 1024;

extern volatile sig_atomic_t g_stop;
void on_signal(int);

inline int64_t now_ns()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
