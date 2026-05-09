# Lock-free stack (Treiber stack with ABA protection)

## Purpose

Replace Win32 `SLIST_HEADER` for the memory pool's free lists. Push/pop
must be lock-free, multi-producer / multi-consumer, and ABA-safe. This is
the highest-risk lock-free structure in the project; getting it wrong
manifests as use-after-free in the allocator under load, which is hellish
to debug.

## Reference origin

- `IOCP_Rookiss/Engine/MemoryPool.h:9, 29` — `SLIST_HEADER _header`,
  `MemoryPoolEntry` (which inherits `SLIST_ENTRY`).
- `IOCP_Rookiss/Engine/MemoryPool.cpp:8, 14, 21, 26` — `InitializeSListHead`,
  `InterlockedPushEntrySList`, `InterlockedPopEntrySList`.

Win32 `SLIST` uses a hardware double-wide CAS (`cmpxchg16b` on x86-64) to
protect against ABA via a 64-bit pointer plus 64-bit sequence counter. The
Linux replacement does not get this for free — we have to engineer it.

## Public API sketch

```cpp
namespace iouring_net::sync {

template <class Node>
requires requires(Node n) { n.next; }            // intrusive: Node has .next
class lock_free_stack {
public:
    void  push(Node* n) noexcept;
    Node* pop()         noexcept;                 // returns nullptr if empty
    bool  empty() const noexcept;

private:
    struct alignas(16) tagged_head {
        Node*    ptr;
        uint64_t tag;
    };
    std::atomic<tagged_head> head_{tagged_head{nullptr, 0}};

    static_assert(std::atomic<tagged_head>::is_always_lock_free,
                  "platform must support 16-byte CAS (cmpxchg16b)");
};

} // namespace iouring_net::sync
```

`Node` is intrusive — the user provides a struct with a `Node* next` field.
The stack does not allocate. (`MemoryPool` provides `MemoryHeader` as the
node type.)

## Linux design

**ABA protection: tagged pointer.** `tagged_head` packs a pointer and a
64-bit tag in a 16-byte aligned struct. Every successful CAS increments
`tag`. ABA — pointer recurs but the queue has been mutated — is impossible
because the tag will not match.

**Compiler/CPU requirements.**
- x86-64: `cmpxchg16b`. Available on every CPU made after 2007 except
  early AMD K8s; `__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16` indicates support.
  `g++` and `clang++` require `-mcx16` or generally `-march=x86-64-v2` or
  later to emit it.
- ARM64: 16-byte CAS is `casp` (release armv8.1+). Older armv8.0 emits a
  call to `__atomic_compare_exchange_16` which libatomic implements with a
  lock — still correct but breaks the lock-free guarantee. CI must verify
  `is_always_lock_free` is true on each target.
- Compiler flags: `-mcx16` on x86-64, `-march=armv8.1-a` (or later) on ARM64.

**Push (Treiber):**
```cpp
void push(Node* n) noexcept {
    tagged_head expected = head_.load(std::memory_order_relaxed);
    tagged_head desired;
    do {
        n->next = expected.ptr;
        desired = {n, expected.tag + 1};
    } while (!head_.compare_exchange_weak(
        expected, desired,
        std::memory_order_release,
        std::memory_order_relaxed));
}
```

**Pop:**
```cpp
Node* pop() noexcept {
    tagged_head expected = head_.load(std::memory_order_acquire);
    tagged_head desired;
    do {
        if (!expected.ptr) return nullptr;
        desired = {expected.ptr->next, expected.tag + 1};
    } while (!head_.compare_exchange_weak(
        expected, desired,
        std::memory_order_acquire,
        std::memory_order_acquire));
    return expected.ptr;
}
```

**Why this is still subtle.**
- `expected.ptr->next` reads the node *after* it has logically been removed
  by another thread. This is the ABA hazard's twin: the node pointer can
  become a dangling pointer between the load and the dereference if the
  node is popped, freed back to the OS, and the page is unmapped.
- For *this project*, that hazard does not bite because:
  - The memory pool's free list never returns memory to the OS — nodes are
    eternal pool slabs.
  - All `Node`s come from the same backing slab and live for the process
    lifetime.
- If we ever reuse this structure for objects that can be freed back to
  the OS, we need hazard pointers or `epoch_based_reclamation`. **Document
  this constraint in the source: this is a free-list stack, not a
  general-purpose stack.**

## Concurrency & ownership

- MPMC. `push` and `pop` are both safe from any thread.
- Memory ordering: release on push, acquire on pop. The release-acquire
  pair is the publication mechanism for the node's payload.
- Liveness: lock-free in the formal sense. Not wait-free — a thread can
  be starved indefinitely under unbounded contention, but in the
  memory-pool workload contention is bounded.

## Test plan

- Unit: 8 threads × 1M push/pop pairs. Final stack matches expected
  count; no node visited twice.
- Stress under TSan: same workload with `-fsanitize=thread`. Must be
  clean.
- Stress under ASan: cannot detect the ABA hazard directly; instead, run
  with a build that pins nodes (the project's actual usage) and confirms
  no use-after-free.
- Targeted ABA test: thread A loads `head` (node X visible), pauses;
  thread B pops X, pushes Y, pops Y, pushes X back; thread A resumes
  with stale `head` matching but tag mismatched — assert CAS fails.
- Lock-free check: `static_assert(std::atomic<tagged_head>::is_always_lock_free)`
  + a runtime assertion at startup.

## Open questions

1. **Hazard pointers / epoch reclamation.** Out of scope for v1 because
   the memory pool's nodes are eternal. Document the constraint and revisit
   if we ever build a general-purpose lock-free queue / stack.
2. **Bounded vs. unbounded.** This is unbounded. The memory pool workload
   is naturally bounded by the number of allocated objects in flight.
3. **Wait-freedom.** No. We accept lock-freedom for v1. If profiling on
   contended workloads shows starvation, consider Michael & Scott's MS
   queue or the Vyukov MPSC queue for the relevant code path — but those
   are queues, not stacks.
4. **`liburcu` or `boost::lockfree::stack`.** Both are battle-tested; we
   could pull one in. Counter-argument: building the structure ourselves
   is the *point* of the project (the executor and primitives are the
   lesson). Decision: roll our own; cite the prior art.
