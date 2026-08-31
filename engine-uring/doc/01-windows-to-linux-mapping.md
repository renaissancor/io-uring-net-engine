# 01 — Windows → Linux API mapping

The single source of truth for "what does this Win32 call become on Linux."
Every API used in the three reference repos is listed here. If a reference
repo uses a Win32 call not in this table, the mapping has a hole and this
doc must be updated before that subsystem is ported.

Reference repos:
- `IOCP_Rookiss` — `~/CLionProjects/IOCP_Rookiss`
- `SelectServer` — `~/CLionProjects/SelectServer`
- `WindowsLibrary` — `~/CLionProjects/WindowsLibrary`

---

## Memory

| Win32                            | Linux / C++                                           | Notes                                                              |
|----------------------------------|-------------------------------------------------------|--------------------------------------------------------------------|
| `_aligned_malloc(size, align)`   | `std::aligned_alloc(align, size)` (C++17)             | C++17 requires `size` to be a multiple of `align`. Round up.       |
| `_aligned_free(p)`               | `std::free(p)`                                        | `aligned_alloc` allocations are freed with regular `free`.         |
| `VirtualAlloc(.., MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE)` | `mmap(NULL, size, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0)` | Used by `StompAllocator` for page-guard debug allocator. |
| `VirtualFree(p, 0, MEM_RELEASE)` | `munmap(p, size)`                                     | Linux requires the original size — record it in the allocator header. |
| `PAGE_SIZE` constant `0x1000`    | `sysconf(_SC_PAGESIZE)`                               | Don't hardcode; query at startup.                                  |
| `malloc` / `free`                | `malloc` / `free`                                     | Same.                                                              |

**Origins:**
- `IOCP_Rookiss/Engine/Memory.cpp:40`, `Engine/Allocator.cpp:21`
- `WindowsLibrary/Library/WinMemory.cpp:18`

---

## Atomics

`lnx::atomic32` / `lnx::atomic64` / `lnx::atomic_ptr` wrap GCC/Clang
`__atomic_*` builtins as the project's low-level Interlocked replacement.
Memory ordering must be made explicit at the wrapper boundary — Win32
`Interlocked*` is implicitly sequentially consistent, while Linux/C++ lets
us choose weaker ordering when it is still correct.

| Win32                                          | Linux / C++                                        |
|------------------------------------------------|----------------------------------------------------|
| `_InterlockedIncrement(p)`                     | `__atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST)`       |
| `_InterlockedDecrement(p)`                     | `__atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST)`       |
| `_InterlockedExchangeAdd(p, n)`                | `__atomic_fetch_add(p, n, __ATOMIC_SEQ_CST)`       |
| `_InterlockedExchange(p, v)`                   | `__atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)`      |
| `_InterlockedCompareExchange(p, exch, comp)`   | `__atomic_compare_exchange_n` wrapped to return the observed old value |
| `_InterlockedIncrement64`, etc.                | `lnx::atomic64` variants                           |
| `_InterlockedExchangePointer`                  | `lnx::atomic_ptr::exchange`                        |
| `_InterlockedCompareExchangePointer`           | `lnx::atomic_ptr::compare_exchange`                |
| `volatile LONG` (Win32 acquire-release)        | plain storage inside `lnx::atomic32`; synchronization comes from `__atomic_*` memory order |

**Memory-order policy for this project:**
- Reference counts: relaxed increment; acq-rel decrement; acquire fence after decrement hits zero.
- Treiber-stack push: release compare-exchange with relaxed failure ordering.
- Treiber-stack pop: acquire compare-exchange.
- Spin loops: explicit `lnx::memory_barrier()` is forbidden without justification in a code comment.

**Origins:**
- `IOCP_Rookiss/Engine/Atomic.h:29-34, 73-78, 117-120`
- `WindowsLibrary/Library/Include/WinAtomic.h:22-47, 66-91, 109-120`

---

## Mutexes (exclusive)

`lnx::mutex` wraps `pthread_mutex_t` directly with default
(`PTHREAD_MUTEX_NORMAL`) attributes. This bypasses `std::mutex` so that
`lock` / `try_lock` / `unlock` can be honestly `noexcept` — pthread is a C
API and cannot throw, while `std::mutex::lock` is specified to throw
`std::system_error`, which would weaken the `Win::Mutex` contract the
wrapper is trying to deliver. Method names match `Win::Mutex` directly.

| Win32                              | Linux                                |
|------------------------------------|--------------------------------------|
| `CRITICAL_SECTION`                 | `pthread_mutex_t` (default attrs = `PTHREAD_MUTEX_NORMAL`) |
| `InitializeCriticalSection`        | `pthread_mutex_init(.., nullptr)`    |
| `InitializeCriticalSectionEx(.., spinCount, 0)` | `pthread_mutex_init(.., &attr)` with `pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP)` (Linux extension) — only if profiling demands it |
| `EnterCriticalSection`             | `pthread_mutex_lock`                 |
| `TryEnterCriticalSection`          | `pthread_mutex_trylock` (returns `0` / `EBUSY`) |
| `LeaveCriticalSection`             | `pthread_mutex_unlock`               |
| `DeleteCriticalSection`            | `pthread_mutex_destroy`              |
| `LockGuard` (custom RAII)          | `lnx::lock_guard` (RAII on `lnx::mutex`) |
| `UniqueLock` (custom moveable)     | `lnx::unique_lock` (movable RAII on `lnx::mutex`) |
| Custom RAII vocabulary (`Win::Mutex`, `Win::LockGuard`, `Win::UniqueLock`) | `lnx::mutex`, `lnx::lock_guard`, `lnx::unique_lock` in `src/sync/mutex.h` (pthread direct) |

**Default:** `lnx::mutex` (pthread direct, `PTHREAD_MUTEX_NORMAL`) everywhere.
Adaptive `pthread_mutex_t` only with measurement. `std::mutex` is not used in
the project; its `system_error`-throwing `lock()` does not fit the
"infallible-or-UB" contract inherited from `Win::Mutex`.

**Origins:**
- `WindowsLibrary/Library/Include/WinMutex.h:7-26, 49-117`

---

## Reader/writer locks

`lnx::shared_mutex` wraps `pthread_rwlock_t` directly with default attributes.
Same rationale as `lnx::mutex`: pthread is C, cannot throw, so the WinAPI
infallibility contract is delivered honestly under `noexcept`. Method names
follow `Win::SharedMutex`: `lock_exclusive` / `try_lock_exclusive` /
`unlock_exclusive` / `lock_shared` / `try_lock_shared` / `unlock_shared`.
Std-style `lock` / `try_lock` aliases are intentionally not provided.

| Win32                              | Linux                                |
|------------------------------------|--------------------------------------|
| `SRWLOCK`                          | `pthread_rwlock_t` (default attrs)   |
| `InitializeSRWLock`                | `pthread_rwlock_init(.., nullptr)`   |
| `AcquireSRWLockExclusive`          | `pthread_rwlock_wrlock`              |
| `TryAcquireSRWLockExclusive`       | `pthread_rwlock_trywrlock`           |
| `ReleaseSRWLockExclusive`          | `pthread_rwlock_unlock`              |
| `AcquireSRWLockShared`             | `pthread_rwlock_rdlock`              |
| `TryAcquireSRWLockShared`          | `pthread_rwlock_tryrdlock`           |
| `ReleaseSRWLockShared`             | `pthread_rwlock_unlock` (same call — POSIX releases whichever mode is held) |
| (no SRWLOCK destroy)               | `pthread_rwlock_destroy`             |
| `SharedLockGuard` (custom)         | `lnx::shared_lock_guard` (RAII on `lnx::shared_mutex`) |
| `ExclusiveLockGuard` (custom)      | `lnx::exclusive_lock_guard` (RAII on `lnx::shared_mutex`) |
| Custom RAII vocabulary (`Win::SharedMutex`, `Win::SharedLockGuard`, `Win::ExclusiveLockGuard`) | `lnx::shared_mutex`, `lnx::shared_lock_guard`, `lnx::exclusive_lock_guard` in `src/sync/mutex.h` (pthread direct) |

**Caveat:** glibc's `pthread_rwlock_t` is writer-preferring. If readers must
be preferred, build a custom rwlock on a futex — out of scope for v1.

**Unified unlock.** POSIX `pthread_rwlock_unlock` releases whichever mode the
calling thread holds, unlike Win's split `ReleaseSRWLockExclusive` /
`ReleaseSRWLockShared`. `lnx::shared_mutex` keeps the two `unlock_*` methods
separate to mirror `Win::SharedMutex`; they both forward to
`pthread_rwlock_unlock`.

**Origins:**
- `IOCP_Rookiss/Engine/SharedMutex.h:5`
- `WindowsLibrary/Library/Include/WinMutex.h:28-47, 119-143`

---

## Lock-free stack (highest-priority replacement)

| Win32                              | Linux / C++                          |
|------------------------------------|--------------------------------------|
| `SLIST_HEADER`                     | custom Treiber stack header (`std::atomic<TaggedPtr>`) |
| `SLIST_ENTRY`                      | `struct Node { Node* next; }`        |
| `InitializeSListHead`              | constructor / aggregate init         |
| `InterlockedPushEntrySList`        | `Stack::push` (CAS loop)             |
| `InterlockedPopEntrySList`         | `Stack::pop` (CAS loop with ABA tag) |

**Critical:** Win32 `SLIST` has hardware-assisted ABA protection on x86-64
(double-wide CAS via `cmpxchg16b`). The Linux replacement must protect
against ABA explicitly — either via tagged pointers (low bits as a
generation counter; requires 16-byte alignment of nodes) or hazard pointers.
This is **the** subtlety that agent-written replacements get wrong.

See `doc/sync/lock_free_stack.md` for the full design.

**Origins:**
- `IOCP_Rookiss/Engine/MemoryPool.h:9`, `Engine/MemoryPool.cpp:8, 14, 21, 26`

---

## Sockets and I/O

| Win32                                          | Linux                                                  |
|------------------------------------------------|--------------------------------------------------------|
| `WSAStartup` / `WSACleanup`                    | not needed                                             |
| `socket(AF_INET, SOCK_STREAM, 0)`              | `socket(AF_INET, SOCK_STREAM \| SOCK_NONBLOCK \| SOCK_CLOEXEC, 0)` |
| `bind`, `listen`                               | `bind`, `listen` (same)                                |
| `accept`                                       | `accept4(.., SOCK_NONBLOCK \| SOCK_CLOEXEC)` (preferred) |
| `closesocket`                                  | `close`                                                |
| `shutdown(s, SD_SEND/RECEIVE/BOTH)`            | `shutdown(s, SHUT_WR/SHUT_RD/SHUT_RDWR)`               |
| `ioctlsocket(s, FIONBIO, &nb)`                 | not needed if using `SOCK_NONBLOCK` at creation        |
| `setsockopt(.., TCP_NODELAY, ..)`              | same                                                   |
| `setsockopt(.., SO_LINGER, ..)`                | same                                                   |
| `setsockopt(.., SO_REUSEADDR, ..)`             | same                                                   |
| `WSAGetLastError()`                            | `errno` (or the negated `cqe->res` from io_uring)      |

**Origins:**
- `SelectServer/FighterOOP/Net.cpp:132-187`

---

## select / IOCP / WSARecv → io_uring

| Win32                                          | Linux io_uring                                                                   |
|------------------------------------------------|----------------------------------------------------------------------------------|
| `select`, `FD_ZERO`, `FD_SET`, `FD_CLR`        | not used; `io_uring` covers the same role                                        |
| `CreateIoCompletionPort` (create)              | `io_uring_queue_init(entries, &ring, flags)` (`liburing`)                        |
| `CreateIoCompletionPort` (associate fd)        | per-op when submitting an SQE; or `io_uring_register_files` for fixed-fd mode    |
| `GetQueuedCompletionStatus(.., INFINITE)`      | `io_uring_wait_cqe(&ring, &cqe)`                                                 |
| `GetQueuedCompletionStatus(.., 0)` (poll)      | `io_uring_peek_cqe(&ring, &cqe)`                                                 |
| `PostQueuedCompletionStatus` (cross-thread wake) | `io_uring_prep_msg_ring` (kernel 5.18+) — preferred. Fallback: `eventfd` registered with `io_uring_register_eventfd`. |
| `WSARecv` (overlapped)                         | `io_uring_prep_recv(sqe, fd, buf, len, flags)`                                   |
| `WSARecvMulti` (multishot recv)                | `io_uring_prep_recv_multishot` (kernel 6.0+) with provided buffers (`IOSQE_BUFFER_SELECT`) |
| `WSASend` (overlapped)                         | `io_uring_prep_send(sqe, fd, buf, len, flags)`                                   |
| `AcceptEx`                                     | `io_uring_prep_accept(sqe, listen_fd, addr, &addrlen, flags)`                    |
| `AcceptEx` multi-accept                        | `io_uring_prep_multishot_accept` (kernel 5.19+)                                  |
| `ConnectEx`                                    | `io_uring_prep_connect(sqe, fd, addr, addrlen)`                                  |
| `WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER)` | not needed                                                                       |
| Per-op overlapped buffer pinning               | `io_uring_register_buffers` + `io_uring_prep_recv_fixed` etc.                    |

**Userdata convention:** every SQE submitted in this project encodes a tagged
pointer in `sqe->user_data` whose low bits identify the op type and high
bits point to the owning session.
See `doc/network/io_uring_reactor.md`.

**Origins:**
- `IOCP_Rookiss/Engine/pch.h:12-14` (winsock includes — declared but unused)
- `SelectServer/FighterOOP/Net.cpp:287-306` (select loop, the model being replaced)

---

## Threading

| Win32                              | Linux / C++                                                  |
|------------------------------------|--------------------------------------------------------------|
| `CreateThread`                     | raw `clone(2)` / `clone3(2)` level; avoid for normal C/C++ threads |
| `_beginthreadex(.., proc, .., &id)` | `pthread_create` via `lnx::thread` (`src/runtime/thread.h`)                    |
| `HANDLE`                            | `pthread_t` inside `lnx::thread`                             |
| `WaitForSingleObject(h, INFINITE)`  | `pthread_join` via `lnx::thread::join`                       |
| `CloseHandle` (thread)              | `pthread_detach` via `lnx::thread::detach`                   |
| `GetCurrentThreadId`                | `gettid()` / `syscall(SYS_gettid)` for Linux kernel TID      |
| `TlsAlloc` / `TlsGetValue` / `TlsSetValue` | `thread_local`                                       |
| `volatile thread_local`             | `thread_local std::atomic<>` if cross-thread visible         |
| Sleep / SleepEx                     | `clock_nanosleep` or `nanosleep`                             |

`pthread_create` is the Linux runtime-aware thread API, closer in spirit
to `_beginthreadex` than to raw Win32 `CreateThread`. Avoid raw `clone`
unless building a thread runtime deliberately.

**Origins:**
- `WindowsLibrary/Library/Include/WinThread.h:8-79`
- `IOCP_Rookiss/Engine/ThreadManager.cpp:8-14` (TLS stubs only)
- `doc/runtime/thread.md`

---

## Timing

| Win32                              | Linux / C++                          |
|------------------------------------|--------------------------------------|
| `QueryPerformanceCounter`          | `clock_gettime(CLOCK_MONOTONIC, &ts)` or `std::chrono::steady_clock::now()` |
| `QueryPerformanceFrequency`        | not needed (`clock_gettime` returns ns directly) |
| `timeBeginPeriod(1)`               | not needed (Linux scheduler does not require this) |
| `Sleep(ms)`                        | `std::this_thread::sleep_for(std::chrono::milliseconds(ms))` |

**Origins:**
- `SelectServer/FighterOOP/main.cpp:38-39, 57, 86`
- `WindowsLibrary/Library/Include/Profiler.h:34, 84`

---

## Errors

Win32 has multiple error mechanisms (`GetLastError`, `WSAGetLastError`,
`HRESULT`, `errno` for CRT). Linux has one: `errno` (and for `io_uring`, the
negated value of `cqe->res`).

This project uses `std::error_code` as the public surface (see
`doc/sync/sync_primitives.md` and `doc/network/io_uring_reactor.md`
for the error category). I/O failures return `expected<T,
std::error_code>` (project alias resolving to `tl::expected` — see
`doc/02-build-and-toolchain.md` polyfill section); bugs throw.

---

## Net new on Linux (no Win32 analog needed)

These are **Linux-specific** syscalls (not portable POSIX) used by this
project that have no Win32 ancestor to map from. On macOS / *BSD they
require alternative APIs (`kqueue`, `dispatch_source_t`, `pthread_setname_np`
with different signatures, etc.) — out of scope for this project, which
is Linux-only:

- `eventfd` — for cross-thread wakeup before kernel 5.18.
- `signalfd` — for handling termination signals inside the reactor.
- `timerfd` — if we ever need a timer wheel inside the reactor.
- `epoll_create1(EPOLL_CLOEXEC)` — only used as a fallback or for hybrid
  designs; primary path is `io_uring`.
- `prctl(PR_SET_NAME, ..)` — naming worker threads for `ps` / `top` / `perf`.
- io_uring runtime feature detection — three-layer probe at startup:
  `io_uring_params.features` for `IORING_FEAT_*` infrastructure bits,
  `io_uring_get_probe()` for opcode availability, and trial-submit for
  per-op flags (`IORING_ACCEPT_MULTISHOT`, `IORING_RECV_MULTISHOT`,
  `IOSQE_BUFFER_SELECT`). FEAT bits alone are insufficient.
  See `doc/02-build-and-toolchain.md` "Three-layer feature detection".

---

## Coverage check

Every API category surfaced by the three Explore agent reports is mapped
above. If you find a Win32 call in the reference repos not represented here,
the gap should be added to this table before the dependent subsystem is
ported.
