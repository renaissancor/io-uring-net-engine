#pragma once
// app/roster.h
//
// THE THREAD ROSTER — the complete, compile-time manifest of every thread this
// process will ever run. Nothing spawns that is not named here.
//
//   supervisor   1, always   main() itself; sole spawner, never runs an engine
//   acceptor     1, always   listen/accept, session authority, handoff producer
//   worker       k_worker_count, 1..8   owns fds + rings + rooms + its io_uring
//   db           0 — deferred to v2 (see project memory "db thread deferred")
//   logger       0 — deferred to v2
//
// The roster is FIXED BEFORE RUNTIME, not merely bounded. This is deliberate:
//
//   * Everything that can fail has failed by the boot barrier. The thread set,
//     the mesh edge set, and every arena size are known at link time, so there
//     is no runtime path that decides how much to allocate.
//   * The mesh becomes a compile-time graph. Edge count is 2 * k_worker_count
//     (one acceptor->worker admission pipe and one worker->acceptor close-notify
//     pipe per worker), so arrays are exact-sized with no slack slots and the
//     fan-out loop has a constexpr bound.
//   * Spawning or retiring a thread on a live server is a latency event — a new
//     TLS block, a fresh arena mmap, a page-fault storm, and scheduler churn
//     across every existing worker's cache and NUMA locality. That is precisely
//     backwards for a server whose thesis is predictable tail latency. Retiring
//     is worse still: a worker's sessions would need fd ownership, ring
//     contents, and room membership migrated, which is world migration — an
//     explicit non-goal (doc/10 §12).
//
// Scale-out beyond k_worker_count is by PROCESS, not by thread.
//
// Tuning is a build-time knob, not a config field: configure with
// `-DIOURING_NET_WORKER_COUNT=4` to produce a variant binary. Benchmarking a
// sweep of worker counts stays cheap; a running server's roster stays fixed.

#include "../types.h"

namespace app::roster {

#ifndef IOURING_NET_WORKER_COUNT
#define IOURING_NET_WORKER_COUNT 1
#endif

// Worker pool size. Set at configure time via -DIOURING_NET_WORKER_COUNT.
inline constexpr i32 k_worker_count = IOURING_NET_WORKER_COUNT;

static_assert(k_worker_count >= 1,
              "the roster needs at least one worker — nothing owns session fds otherwise");
static_assert(k_worker_count <= 8,
              "worker thread names are worker_0..worker_7 (8 chars, matching "
              "\"acceptor\" / \"database\"); beyond 8, scale horizontally by process");

// Mesh edges implied by the roster: one admission pipe + one close-notify pipe
// per worker. Asserted against the wiring in main.cpp.
inline constexpr i32 k_mesh_edge_count = 2 * k_worker_count;

// Every thread in the process: supervisor + acceptor + the worker pool.
inline constexpr i32 k_thread_count = 1 + 1 + k_worker_count;

}  // namespace app::roster
