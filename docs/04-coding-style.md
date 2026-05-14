# 04 — Coding style

Project-wide conventions: namespace, naming, file layout, and hot-path
rules. Reference document; consult before adding any new file.

---

## Namespace

**All public library types live under `iouring_net::`** as the
umbrella namespace. This matches the install include path
(`<prefix>/include/iouring_net/...`), the CMake target alias
(`iouring_net::iouring_net`), and the consumer-side spelling
(`iouring_net::session`, `iouring_net::task<T>`, etc.) used
throughout `iouring-net-server`.

Earlier drafts placed bare classes at **global scope** to match the
WindowsLibrary convention (`RingBuffer` not `Win::RingBuffer`). That
convention works for monolithic projects but conflicts with the
`find_package` contract for a library consumed across repository
boundaries; the change to `iouring_net::` as the umbrella is
deliberate and resolves the namespace contract drift surfaced in
the architectural review.

Within `iouring_net::`, **sub-namespaces are reserved for genuine
taxonomies**, not for grouping files in the same `src/` folder:

```cpp
namespace iouring_net {

  // Raw POSIX/Linux API wrappers (the Linux equivalent of std::).
  // Brand for code that directly touches pthread_mutex_t, __atomic_*,
  // eventfd, io_uring_*, etc.
  namespace lnx {
      class mutex, shared_mutex;
      class atomic32, atomic64, atomic_ptr;
      class lock_guard, unique_lock, shared_lock_guard, exclusive_lock_guard;
      // future: file, socket, eventfd, timerfd, signalfd, pipe,
      //         clock, mmap_region, ...
  }

  // Generic data structures — the "sds" personal-library brand.
  // Independent of iouring_net domain concepts.
  namespace sds {
      class ring_buffer;
      template<typename V> class cstr_hash_map;
      template<typename T> class indexed_heap;
      template<typename T> class malloc_vector;
      class serial_buffer;
  }

  // Diagnostic subsystems with a manager singleton plus helpers.
  // Matches WindowsLibrary's NewTracer::, GuardOverflow::, Profiler::
  // pattern, nested one level deeper under iouring_net::.
  namespace leak_tracker      { struct info; class manager;
                                /* operator new / delete overrides */ }
  namespace profiler          { enum class time_unit; struct record;
                                struct summary_data; class manager; class scope; }
  namespace deadlock_profiler { class manager; }
  namespace guard_overflow    { class manager; /* alloc, free free functions */ }
  namespace log               { enum class level; class logger; }

  // Direct members of iouring_net::  — primitives, runtime, network
  // surface. No sub-namespace needed; these ARE iouring_net.
  class memory_pool;
  template<typename T> class object_pool;
  template<typename T> class lock_free_stack;

  template<typename T> class task;
  class reactor;
  class job_queue;
  class thread_context;

  class listener;
  class service;
  class session;
  class session_handle;
  class packet_framing;
  struct frame_view;
  enum class close_reason;
  // packet_handler: deferred — dispatcher lives in iouring-net-server

} // namespace iouring_net
```

**Public spelling.** External consumers always write
`iouring_net::session`, `iouring_net::session_handle`,
`iouring_net::task<T>`, etc. Internal-to-library code may
`using namespace iouring_net` in a `.cpp` file for brevity but
**never** in a header.

**Why `lnx::` is narrow.** `lnx::` brands code that *directly wraps* raw
POSIX or Linux-specific APIs (`pthread_mutex_t`, `__atomic_*`, `eventfd`,
`io_uring_*`). Abstract code that only *uses* `lnx::` types internally
stays at global scope (or in its own per-subsystem namespace if it groups
multiple types). This mirrors the WindowsLibrary precedent: `Win::` held
`Atomic32` / `Mutex`; `RingBuffer` and `cstr_hash_map` were global.

**Folder is independent of namespace.** `src/sync/` houses both
`lnx::mutex` (raw API) and global `lock_free_stack` (abstract data
structure built on `lnx::atomic*`). Folders organize *function*;
namespaces organize *API tier*. They are intentionally orthogonal.

---

## Naming

| Kind | Convention | Example |
|---|---|---|
| Class / struct | `snake_case` | `ring_buffer`, `memory_pool`, `session` |
| Method / free function | `snake_case` | `enqueue`, `peek_frame`, `compare_exchange` |
| Private member | `_snake_case` | `_buffer`, `_capacity`, `_head` |
| Public member | `snake_case` | `count`, `next` |
| Local variable | `snake_case` | `bytes_read`, `new_capacity` |
| `static constexpr` constant | `SCREAMING_SNAKE_CASE` | `MAX_SEGMENT_SIZE`, `BUFFER_CAPACITY` |
| Namespace | `lowercase` | `lnx`, `leak_tracker`, `profiler` |
| Template parameter | `PascalCase`, single letter when generic | `T`, `V`, `Promise`, `Args...` |
| Enum class value | `snake_case` | `time_unit::nanosec` |
| File name | `snake_case.h` / `.cpp` | `ring_buffer.h`, `memory_pool.cpp` |
| Macro (rare; avoid) | `LNX_SCREAMING_SNAKE` | `LNX_LIKELY(x)` |

Header guard: `#pragma once` only. First non-blank line after the
`#pragma once` is a single-line `// <filename>.h` comment, matching the
existing `WindowsLibrary` headers.

**No PascalCase.** POSIX, glibc, the Linux kernel, `liburing`, and `std::`
are all snake_case; matching that keeps `lnx::session` and
`io_uring_submit_sqe()` legible side by side. CUDA is the documented
exception in the C++ ecosystem; this project does not ship CUDA.

---

## File and folder layout

Flat tree under `src/`. Headers and sources sit together — no separate
`include/` directory. Per-source-file design docs live under `wiki/`,
mirroring `src/`. Library-wide docs (architecture, build, conventions)
live under `docs/`.

```
iouring-net-lib/
├── src/
│   ├── data_structure/   ring_buffer, serial_buffer, cstr_hash_map,
│   │                     indexed_heap, malloc_vector
│   ├── memory/           memory_pool, object_pool, leak_tracker,
│   │                     guard_overflow
│   ├── sync/             atomic, mutex, shared_mutex, lock_free_stack
│   ├── diagnostic/       logger, deadlock_profiler, profiler
│   ├── runtime/          task, reactor, job_queue, thread_context
│   └── network/          listener, service, session, session_handle,
│                         packet_framing
│                         (packet_handler deferred — product-side)
├── tests/                mirrors src/ structure
├── examples/echo_server/
├── benchmarks/
├── docs/                 library-wide documentation
│   └── 00-overview.md ... 08-test-strategy.md
├── wiki/                 per-source-file design docs (mirror of src/)
│   ├── data_structure/<name>.md
│   ├── memory/<name>.md
│   ├── sync/<name>.md
│   ├── diagnostic/<name>.md
│   ├── runtime/<name>.md
│   └── network/<name>.md
└── CMakeLists.txt
```

Two-tier documentation:
- **`docs/`** — library-wide. Numbered top-level docs covering scope
  (`00`), Win32 mapping (`01`), build/kernel/deps (`02`), this style
  guide (`04`), CMake (`05`), system setup (`06`), CI and
  reproducibility (`07`), and test strategy (`08`). The `03` slot is
  empty (a retrospective journal was folded in and dropped). See
  `docs/README.md` for the full index.
- **`wiki/`** — per-file. Each meaningful source file has a peer design
  doc at `wiki/<category>/<name>.md` describing rationale, invariants,
  reference-repo origin, and any quirks of that one file. See
  `wiki/README.md` for the wiki ↔ src/ mapping table.

`#include` paths are project-relative inside `src/`:

```cpp
#include "data_structure/ring_buffer.h"
#include "memory/memory_pool.h"
```

The CMake target adds `src/` to its include directories.

---

## Hot-path conventions

The hot path is per-connection recv/send dispatch. Everything else is
"warm" (per-connect/disconnect) or "cold" (setup, teardown, configuration).

**Layout and aliasing**
- `alignas(64)` on shared atomic state to prevent false sharing on
  x86_64. Matches `WinAtomic.h:8`.
- `volatile` only inside atomic wrappers where the wrapper requires it
  (`atomic32::_value`); never as a substitute for memory-order semantics.

**Errors and exceptions**
- Hot-path methods are `noexcept`.
- I/O failures return `expected<T, std::error_code>` (project alias —
  see "Project type aliases" below).
- Programming errors throw — but only off the hot path (setup, parsing,
  configuration).
- "If `bad_alloc`, terminate" is acceptable for primitive containers;
  call it out in a comment on the constructor that allocates.

**Project type aliases.** `src/error/expected.h` re-exports the
polyfills as bare names so call sites stay future-proof:

```cpp
// src/error/expected.h
#pragma once
#include <tl/expected.hpp>

template <class T, class E>
using expected = tl::expected<T, E>;

template <class E>
using unexpected = tl::unexpected<E>;
```

When C++23 / libstdc++-15+ is the floor, the body becomes a `using`
declaration over `std::expected` / `std::unexpected` and no call site
changes. Always write `expected` / `unexpected` in headers and source —
never `tl::expected` or `std::expected` directly.

**Allocation**
- No hidden allocations on the hot path. Per-connection state goes
  through `memory_pool` (global namespace).
- `new` / `delete` are intercepted by `leak_tracker::manager` in debug
  builds. The tracker's storage uses `malloc_vector` to avoid recursive
  allocation. Code reachable from `operator new` MUST NOT itself call
  `new` — use `malloc_vector` or stack/static storage.

**Construction**
- Rule of 5 always explicit. Copy and move are `= delete` unless
  ownership is clearly defined.
- Singletons use a private constructor + `static instance()` accessor
  (matches `Profiler::Manager`, `NewTracer::Manager` patterns).

**Inlining**
- Header-only methods get `inline` explicitly even when defined in-class
  (matches existing `RingBuffer.h` / `SerialBuffer.h` style; redundant by
  language rule, but kept for consistency).
- Source-file methods do not get `inline` unless they are header-defined
  templates.

---

## C++ standard

- **C++20. Locked.** Coroutines, concepts, ranges, `std::jthread`,
  `std::stop_token`, designated initializers, `<bit>`.
- **`<format>` is not part of the baseline.** libstdc++-12 (the floor)
  ships an incomplete `std::format`. All formatted output goes through
  `{fmt}` (`fmt::format`, `fmt::print`, `fmt::println`).
- **No C++23.** Two C++23-shaped APIs the design uses are provided by
  vendored polyfills:
  - `expected<T, E>` — `tl::expected` (Sy Brand, header-only).
  - `print` / `println` — `{fmt}` (`fmt::print`, `fmt::println`).
  Both are drop-in API-compatible with the C++23 stdlib equivalents,
  so a future migration to `std::expected` / `std::print` is a typedef
  swap.

Prefer `std::` over project-rolled where semantics match. The wrapper
pattern exists where the platform primitive (spin count, fixed
cache-line alignment, future `futex` use) is exposed deliberately, not
because `std::` is wrong.

| Use `std::` | Wrap or reimplement |
|---|---|
| `std::unique_ptr`, `std::shared_ptr` | — |
| `std::array`, `std::span`, `std::string_view` | — |
| `std::atomic<T>` (off hot path) | `lnx::atomic32` / `atomic64` / `atomic_ptr` (hot path) |
| `std::mutex`, `std::shared_mutex` (off hot path) | `lnx::mutex` (`pthread_mutex` adaptive spin), `lnx::shared_mutex` |
| `std::vector`, `std::unordered_map` (off hot path) | `malloc_vector`, `cstr_hash_map` (hot path or `new`-override-safe) |

---

## What this project does NOT do

- PascalCase identifiers
- `#ifdef _WIN32` shims, `#pragma comment(lib, ...)`, or other
  Windows-isms
- Hungarian notation, `m_`, `g_` — private members use a leading
  underscore (`_buffer`); globals are avoided in favor of singleton
  accessors
- Header-only "include this and you get everything" master headers —
  each TU includes only what it uses

---

## Review checklist (before merging a new file)

- [ ] Symbols placed per namespace tier — `lnx::` for raw POSIX/Linux
      API wrappers, global for pure data + compound primitives +
      single-class subsystems, per-subsystem namespace
      (`leak_tracker::`, `profiler::`, `log::`, etc.) for diagnostics
      that group a `manager` plus helper types
- [ ] Class, method, member names are `snake_case` (members `_snake_case`)
- [ ] `static constexpr` constants are `SCREAMING_SNAKE_CASE`
- [ ] Hot-path methods are `noexcept`
- [ ] Rule of 5 explicit (`= delete` or implemented)
- [ ] `#pragma once` at top; first non-blank line is a `// <filename>.h`
      comment
- [ ] Header and source live in the same `src/<category>/` directory
- [ ] Peer design doc exists at `wiki/<category>/<name>.md`
