# 04 — Coding style

Project-wide conventions: namespace, naming, file layout, and hot-path
rules. Reference document; consult before adding any new file.

---

## Namespace

**Top-level namespaces are flat — there is no umbrella namespace.**
Symbols are grouped by *role*, not by project name. The Linux-side
layering is four primary tiers plus a set of diagnostic subsystems,
all top-level peers:

```cpp
namespace lnx { ... }    // OS primitives
namespace sds { ... }    // Generic data structures (independent personal library)
namespace mem { ... }    // Memory pools
namespace app { ... }    // Domain / runtime — workers, supervisors, sessions

// Diagnostic subsystems (each groups a manager singleton with helper types):
namespace profiler          { ... }
namespace leak_tracker      { ... }
namespace deadlock_profiler { ... }
namespace guard_overflow    { ... }
namespace log               { ... }
```

There is no `iouring_net::` umbrella (rejected 2026-05-23 — short call
sites, no ceremony, survives portfolio expansion to MMO + renderer).
Consumers write `app::session`, `lnx::mutex`, `sds::ring_buffer`,
`mem::packet_pool` directly.

### `lnx::` — OS primitives

Brand for code that directly wraps Linux/POSIX APIs (`pthread_*`,
`__atomic_*`, `eventfd`, `io_uring_*`, `clock_gettime`, etc.).

```cpp
namespace lnx {
    class mutex, shared_mutex;
    class atomic32, atomic64, atomic_ptr;
    class lock_guard, unique_lock, shared_lock_guard, exclusive_lock_guard;
    class thread;
    // future: file, socket, eventfd, timerfd, signalfd, pipe,
    //         clock, mmap_region, ...
}
```

### `sds::` — Specialized Data Structures

Generic data structures, treated as an independent personal library.
No coupling to io_uring, networking, or any domain concept.

```cpp
namespace sds {
    class ring_buffer;
    template<typename V> class cstr_hash_map;
    template<typename T> class indexed_heap;
    template<typename T> class malloc_vector;
    template <typename T, std::size_t N> class spsc_queue;  // concurrent SPSC FIFO; uses lnx::atomic
    class serial_buffer;
}
```

### `mem::` — Memory pools

Allocator and pool subsystems.

```cpp
namespace mem {
    class packet_pool;                           // TLS singleton, mmap-backed
    // future: template<typename T> class object_pool;
}
```

### `app::` — Domain / runtime

Application-level types: workers, supervisors, sessions, rooms,
packets, message types, supervisor orchestration. Survives portfolio
expansion (chat → MMO → renderer).

```cpp
namespace app {
    class supervisor;
    class worker;
    class session;
    class session_handle;
    struct packet_header;
    struct frame_view;
    enum class close_reason;
    // ... domain types
}
```

### Top-level diagnostic subsystems

All diagnostic subsystems sit at top level (peers of `lnx::`, `sds::`,
`mem::`, `app::`), each grouped around a `manager` singleton plus
helper types:

```cpp
namespace profiler          { enum class time_unit; struct record;
                              struct summary_data; class manager; class scope; }
namespace leak_tracker      { struct info; class manager;
                              /* operator new / delete overrides */ }
namespace deadlock_profiler { class manager; }
namespace guard_overflow    { class manager; /* alloc / free helpers */ }
namespace log               { enum class level; class logger; }
```

Folder layout (`src/diagnostic/`) groups them on disk; the namespace
tree doesn't need to. Whether to introduce a `diag::` parent for the
diagnostic family is deferred — revisit when the full diagnostic set
is in view.

### Sub-namespaces — when to nest

Nest only for **genuine taxonomy**, not "files in the same folder."
No subsystems are nested today. The bar is high — prefer a top-level
namespace over nesting unless the nested subsystem is tightly coupled
to its parent.

### Internal vs public spelling

External consumers always write the fully qualified name:
`app::session`, `lnx::mutex`, `sds::ring_buffer`, `mem::packet_pool`.
Internal-to-library code may `using namespace app;` (etc.) in a
`.cpp` file for brevity but **never** in a header.

### Folder is independent of namespace

`src/sync/` houses both `lnx::mutex` (raw API) and `lnx::atomic*`
(both wrap POSIX/builtin primitives). `src/diagnostic/` houses
multiple top-level diagnostic namespaces (`profiler::`,
`leak_tracker::`, `log::`, etc.). Folders organize *function*;
namespaces organize *API tier and contract*. They are intentionally
orthogonal.

### CMake target name

The CMake export target remains `iouring_net::iouring_net` (matches
the repo name `iouring-net-lib`). This is independent of the C++
namespace tree — CMake target naming and C++ namespacing don't have
to match. cf. Boost (`Boost::boost` CMake → `boost::` C++).

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
| Namespace | `lowercase` | `lnx`, `app`, `sds`, `mem`, `leak_tracker` |
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
`include/` directory. Per-source-file design docs live under `doc/`,
mirroring `src/`. Library-wide docs (architecture, build, conventions)
live under `doc/`.

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
├── doc/                 library-wide documentation
│   └── 00-overview.md ... 08-test-strategy.md
├── doc/                 per-source-file design docs (mirror of src/)
│   ├── data_structure/<name>.md
│   ├── memory/<name>.md
│   ├── sync/<name>.md
│   ├── diagnostic/<name>.md
│   ├── runtime/<name>.md
│   └── network/<name>.md
└── CMakeLists.txt
```

Two-tier documentation:
- **`doc/`** — library-wide. Numbered top-level docs covering scope
  (`00`), Win32 mapping (`01`), build/kernel/deps (`02`), this style
  guide (`04`), CMake (`05`), system setup (`06`), CI and
  reproducibility (`07`), and test strategy (`08`). The `03` slot is
  empty (a retrospective journal was folded in and dropped). See
  `doc/README.md` for the full index.
- **`doc/`** — per-file. Each meaningful source file has a peer design
  doc at `doc/<category>/<name>.md` describing rationale, invariants,
  reference-repo origin, and any quirks of that one file. See
  `doc/README.md` for the wiki ↔ src/ mapping table.

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
  through `mem::` pools (`mem::packet_pool` today; future
  `mem::object_pool`).
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

- **C++20. Locked.** Concepts, ranges, `std::jthread`,
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

## `std::` namespace policy

**Goal: nearly not using STL.** The project prefers self-rolled or
POSIX-direct equivalents wherever it matters. A small set of `std::`
types is permitted because they're essentially aliases or compile-time
constructs that don't drag in allocator/exception machinery.

### Permitted `std::` usage

| Category | Examples | Why OK |
|---|---|---|
| Type traits | `std::is_trivially_copyable_v`, `std::is_standard_layout_v`, `std::declval`, `std::remove_cv_t` | Compile-time only; zero runtime cost; no allocator |
| Typedef aliases | `std::size_t`, `std::byte`, `std::uint8_t`, `std::int32_t`, `std::uint64_t`, `std::ptrdiff_t`, `std::nullptr_t` | Aliases for `<cstdint>` / `<cstddef>` types — same machine code |
| C-library funcs | `std::memcpy`, `std::memset`, `std::memmove`, `std::memcmp`, `std::aligned_alloc`, `std::free` | C ABI; no allocator, no exceptions, no hidden state |
| View types | `std::span<T>`, `std::string_view` | Pointer + length; no allocation; no exceptions |
| `<bit>` helpers | `std::countr_zero`, `std::popcount`, `std::bit_cast`, `std::has_single_bit` | Compile to single instructions; no runtime cost |

### Banned `std::` usage

| Category | Banned | Replacement |
|---|---|---|
| Owning containers | `std::vector`, `std::string`, `std::array`(*), `std::map`, `std::unordered_map`, `std::list`, `std::deque`, `std::set`, `std::queue` | `sds::malloc_vector`, `sds::cstr_hash_map`, `sds::indexed_heap`, project-rolled. Raw `T arr[N]` + size for fixed-cap collections |
| Smart pointers | `std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr` | Manual `new`/`delete` with `LNX_CHECK` invariants, or arena-backed allocation. Architecture v1 avoids `shared_ptr` entirely |
| Callables | `std::function`, `std::bind` | Function pointers, template parameters, or call-site lambdas |
| Sync primitives | `std::mutex`, `std::shared_mutex`, `std::atomic<T>`, `std::thread`, `std::jthread`, `std::condition_variable`, `std::latch`, `std::barrier` | `lnx::mutex`, `lnx::shared_mutex`, `lnx::atomic_*`, `lnx::thread` (`src/sync/`, `src/runtime/thread.h`) |
| Error / fatal | `std::abort`, `std::terminate`, `std::system_error`, `std::error_code`, `std::exception`, all of `<stdexcept>` | `LNX_TRAP()` / `LNX_CHECK` for fatals; `tl::expected<T, E>` for recoverable errors. Project bans exceptions. |
| Streams | `std::cout`, `std::cerr`, `std::cin`, `std::ostream`, `std::ifstream` | `fmt::print` / `fmt::println` for stdout/stderr; raw `read()` / `write()` or `fopen` / `fwrite` for files |
| Format | `std::format`, `std::print`, `std::println` | `fmt::format` / `fmt::print` / `fmt::println` (`{fmt}` is in the toolchain) |

*`std::array<T, N>` is a borderline case; banned for stylistic consistency
with the rest of the policy (project prefers raw `T arr[N]` + a
compile-time `N` constant). Not a hard ban — flag if a reason arises to
revisit.

**Why this rule.** `std::` containers couple the codebase to libstdc++'s
allocator/exception model. `std::mutex::lock()` is specified to throw
`std::system_error` — a `noexcept` wrapper around it is either a lie or
forced to break its WinAPI-port contract. `std::vector::push_back` can
throw `std::bad_alloc`. Every owning std:: container internally calls
`operator new`. None of this is compatible with the project's
no-exceptions floor. Sync-primitive scope was decided 2026-05-15
(commit `73098157`); extended project-wide 2026-05-19.

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

- [ ] Symbols placed per namespace tier — `lnx::` for OS primitive
      wrappers; `sds::` for generic data structures; `mem::` for memory
      pools; `app::` for domain/runtime (worker, supervisor, session,
      etc.); top-level `profiler::`, `leak_tracker::`, `log::`,
      `deadlock_profiler::`, `guard_overflow::` for diagnostic
      subsystems
- [ ] Class, method, member names are `snake_case` (members `_snake_case`)
- [ ] `static constexpr` constants are `SCREAMING_SNAKE_CASE`
- [ ] Hot-path methods are `noexcept`
- [ ] Rule of 5 explicit (`= delete` or implemented)
- [ ] `#pragma once` at top; first non-blank line is a `// <filename>.h`
      comment
- [ ] Header and source live in the same `src/<category>/` directory
- [ ] Peer design doc exists at `doc/<category>/<name>.md`
