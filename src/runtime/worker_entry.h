#pragma once
// runtime/worker_entry.h
//
// Worker-thread entry trampoline. Per the chat-server v1 boot order
// (see .omc/wiki/memory-pool-tls-singleton-mmap-design-decision.md):
//
//   [0] anchor mem::packet_pool::instance() — mmaps the region
//   [1] packet_pool::prewarm()              — populate free lists
//   [2] io_uring init + run                 — worker's own start fn
//
// Use this as the pthread start routine for worker threads. The real
// worker body is passed via `worker_start`. Non-worker threads
// (tests, the DB thread) should not go through this trampoline —
// they don't need the per-thread packet pool.

#include "memory/packet_pool.h"

namespace lnx {

struct worker_start {
    void* (*fn)(void*);
    void*  arg;
};

inline void* worker_entry(void* p) noexcept {
    auto* start = static_cast<worker_start*>(p);
    mem::packet_pool::instance().prewarm();
    return start->fn(start->arg);
}

}  // namespace lnx
