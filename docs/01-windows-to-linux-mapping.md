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

`std::atomic<T>` replaces every `Interlocked*` intrinsic. Memory ordering
must be made explicit — Win32 `Interlocked*` is implicitly sequentially
consistent; on Linux/C++ we choose the weakest ordering that is still
correct.

| Win32                                          | C++ `std::atomic`                                  |
|------------------------------------------------|----------------------------------------------------|
| `_InterlockedIncrement(p)`                     | `std::atomic<int32_t>::fetch_add(1) + 1`           |
| `_InterlockedDecrement(p)`                     | `std::atomic<int32_t>::fetch_sub(1) - 1`           |
| `_InterlockedExchangeAdd(p, n)`                | `std::atomic<>::fetch_add(n)`                      |
| `_InterlockedExchange(p, v)`                   | `std::atomic<>::exchange(v)`                       |
| `_InterlockedCompareExchange(p, exch, comp)`   | `std::atomic<>::compare_exchange_strong(comp, exch)` (returns bool; old value is in `comp` on failure) |
| `_InterlockedIncrement64`, etc.                | `std::atomic<int64_t>` variants                    |
| `_InterlockedExchangePointer`                  | `std::atomic<T*>::exchange`                        |
| `_InterlockedCompareExchangePointer`           | `std::atomic<T*>::compare_exchange_strong`         |
| `volatile LONG` (Win32 acquire-release)        | `std::atomic<int32_t>` with explicit `memory_order` |

**Memory-order policy for this project:**
- Reference counts: `fetch_add(memory_order_relaxed)` for inc, `fetch_sub(memory_order_acq_rel)` for dec, `acquire` fence after dec hits zero.
- Treiber-stack push: `compare_exchange_weak(memory_order_release, memory_order_relaxed)`.
- Treiber-stack pop: `compare_exchange_weak(memory_order_acquire, memory_order_acquire)`.
- Spin loops: explicit `std::atomic_thread_fence(memory_order_seq_cst)` is forbidden without justification in a code comment.

**Origins:**
- `IOCP_Rookiss/Engine/Atomic.h:29-34, 73-78, 117-120`
- `WindowsLibrary/Library/Include/WinAtomic.h:22-47, 66-91, 109-120`

---

## Mutexes (exclusive)

| Win32                              | Linux / C++                          |
|------------------------------------|--------------------------------------|
| `CRITICAL_SECTION`                 | `std::mutex`                         |
| `InitializeCriticalSectionEx(.., spinCount, 0)` | adaptive: `pthread_mutex_t` with `PTHREAD_MUTEX_ADAPTIVE_NP` (Linux extension) — only if profiling shows `std::mutex` is contended hot |
| `EnterCriticalSection`             | `std::mutex::lock`                   |
| `TryEnterCriticalSection`          | `std::mutex::try_lock`               |
| `LeaveCriticalSection`             | `std::mutex::unlock`                 |
| `DeleteCriticalSection`            | destructor                           |
| `LockGuard` (custom RAII)          | `std::lock_guard` / `std::scoped_lock` |
| `UniqueLock` (custom moveable)     | `std::unique_lock`                   |

**Default:** `std::mutex` everywhere. Only switch to adaptive `pthread_mutex_t`
with measurement.

**Origins:**
- `WindowsLibrary/Library/Include/WinMutex.h:7-26, 49-117`

---

## Reader/writer locks

| Win32                              | Linux / C++                          |
|------------------------------------|--------------------------------------|
| `SRWLOCK`                          | `std::shared_mutex` (C++17)          |
| `InitializeSRWLock`                | constructor                          |
| `AcquireSRWLockExclusive`          | `std::shared_mutex::lock`            |
| `TryAcquireSRWLockExclusive`       | `std::shared_mutex::try_lock`        |
| `ReleaseSRWLockExclusive`          | `std::shared_mutex::unlock`          |
| `AcquireSRWLockShared`             | `std::shared_mutex::lock_shared`     |
| `TryAcquireSRWLockShared`          | `std::shared_mutex::try_lock_shared` |
| `ReleaseSRWLockShared`             | `std::shared_mutex::unlock_shared`   |
| `SharedLockGuard` (custom)         | `std::shared_lock`                   |
| `ExclusiveLockGuard` (custom)      | `std::lock_guard<std::shared_mutex>` |

**Caveat:** `std::shared_mutex` is writer-preferring on libstdc++ (uses
glibc's pthread rwlock). If readers must be preferred, build a custom rwlock
on `std::condition_variable` — out of scope for v1.

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

See `wiki/sync/lock_free_stack.md` for the full design.

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
bits point to the owning coroutine handle (or session, depending on doc).
See `wiki/network/io_uring_reactor.md`.

**Origins:**
- `IOCP_Rookiss/Engine/pch.h:12-14` (winsock includes — declared but unused)
- `SelectServer/FighterOOP/Net.cpp:287-306` (select loop, the model being replaced)

---

## Threading

| Win32                              | Linux / C++                                                  |
|------------------------------------|--------------------------------------------------------------|
| `_beginthreadex(.., proc, .., &id)` | `std::jthread{proc}` (preferred — auto-joins, has `stop_token`) |
| `HANDLE`                            | not exposed; use `std::jthread`                              |
| `WaitForSingleObject(h, INFINITE)`  | `std::jthread::join`                                         |
| `CloseHandle` (thread)              | not needed (RAII)                                            |
| `GetCurrentThreadId`                | `std::this_thread::get_id` (opaque) or `gettid()` (Linux tid for tracing) |
| `TlsAlloc` / `TlsGetValue` / `TlsSetValue` | `thread_local`                                       |
| `volatile thread_local`             | `thread_local std::atomic<>` if cross-thread visible         |
| Sleep / SleepEx                     | `std::this_thread::sleep_for` / `clock_nanosleep`            |

**Origins:**
- `WindowsLibrary/Library/Include/WinThread.h:8-79`
- `IOCP_Rookiss/Engine/ThreadManager.cpp:8-14` (TLS stubs only)

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
`wiki/sync/sync_primitives.md` and `wiki/network/io_uring_reactor.md`
for the error category). I/O failures return `expected<T,
std::error_code>` (project alias resolving to `tl::expected` — see
`docs/02-build-and-toolchain.md` polyfill section); bugs throw.

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
  See `docs/02-build-and-toolchain.md` "Three-layer feature detection".

---

## Coverage check

Every API category surfaced by the three Explore agent reports is mapped
above. If you find a Win32 call in the reference repos not represented here,
the gap should be added to this table before the dependent subsystem is
ported.
