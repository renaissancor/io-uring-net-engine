# 04 — Coding style

Project-wide conventions: namespace, naming, file layout, and hot-path
rules. Reference document; consult before adding any new file.

---

## Namespace

**All public library types live under `iouring_net::`** as the
umbrella namespace. This matches the install include path
(`<prefix>/include/iouring_net/...`), the CMake target alias
(`iouring_net::iouring_net`), and the consumer-side spelling
(`iouring_net::session`, `iouring_net::channel`, etc.) used
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
`iouring_net::channel`, etc. Internal-to-library code may
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
