// conn.h — per-connection state and the process-wide clock and stop flag.
//
// Shared by both phases so they cannot drift into two definitions of what a
// connection is. Nothing subtle lives here; if something subtle appears, it
// probably belongs in the phase that needs it.
#pragma once

#include <csignal>
#include <cstdint>
#include <ctime>
#include <string>

// ------------------------------------------------------------------- state

enum class conn_state : uint8_t { none, connecting, ready, dead };

struct conn {
    conn_state  state = conn_state::none;
    int         index = -1;   // logical client id, for nick/room
    uint32_t    seq   = 0;
    std::string pending;      // unsent tail
    std::string in;           // accumulated recv bytes, may hold a partial frame
};

extern volatile sig_atomic_t g_stop;
void on_signal(int);

inline int64_t now_ns()
{
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
