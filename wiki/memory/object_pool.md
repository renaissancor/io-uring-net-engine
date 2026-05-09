# ObjectPool — typed wrapper over the memory pool

## Purpose

A typed, intrusive-style pool layered on top of `MemoryPool`. Hands out
`std::shared_ptr<T>` whose deleter returns the object to the pool instead of
calling `delete`. Used for short-lived, frequently-recycled objects like
`Session`, `Packet`, and per-job task structs.

Conceptually the same shape as the reference `ObjectPool<T>` but expressed
in idiomatic modern C++ (CTAD, perfect forwarding, `std::shared_ptr` with
custom deleter).

## Reference origin

- `IOCP_Rookiss/Engine/ObjectPool.h:8` — templated `ObjectPool<T>` with
  `Pop()` / `Push(T*)` / `MakeShared()`.
- `IOCP_Rookiss/Engine/Allocator.h:19` — STL allocator adapter; serves a
  related role (vector/map allocations) and is documented here too.

## Public API sketch

```cpp
namespace iouring_net::mem {

template <class T>
class ObjectPool {
public:
    template <class... Args>
    static std::shared_ptr<T> make_shared(Args&&... args);

    template <class... Args>
    static T* pop(Args&&... args);                 // raw, manual return
    static void push(T* p);                        // ~T() + release to pool
};

template <class T>
struct PoolAllocator {                              // STL allocator adapter
    using value_type = T;
    T* allocate(size_t n);
    void deallocate(T* p, size_t n);
};

} // namespace iouring_net::mem
```

`ObjectPool<T>::make_shared` is the canonical constructor. The returned
`shared_ptr` carries a deleter that calls `~T()` then `mem::release`. This
preserves `enable_shared_from_this` semantics and works with `std::weak_ptr`.

## Linux design

**Storage backing.** Every `ObjectPool<T>::pop` calls `mem::alloc(sizeof(T))`,
which routes to the bucket sized for `T`. There is no separate per-type free
list — the size class is the pool. This is identical to the reference design
and avoids per-type bookkeeping.

**Construction.** Placement-new at the returned pointer:
```cpp
template <class T, class... Args>
T* pop(Args&&... args) {
    void* raw = mem::alloc(sizeof(T));
    return new (raw) T(std::forward<Args>(args)...);
}
```

**Destruction.**
```cpp
template <class T>
void push(T* p) {
    p->~T();
    mem::release(p);
}
```

**`make_shared` deleter.** Plain lambda, captures nothing:
```cpp
auto deleter = [](T* p) { ObjectPool<T>::push(p); };
return std::shared_ptr<T>(pop(std::forward<Args>(args)...), deleter);
```

**`PoolAllocator<T>`.** Implements the C++17 minimal allocator concept so
`std::vector<T, PoolAllocator<T>>` and friends route through the pool. Used
for `JobQueue`'s internal vector and a few telemetry containers.

## Concurrency & ownership

- `ObjectPool<T>` is stateless (all functions static); thread-safety derives
  entirely from `MemoryPool`'s lock-free free list.
- `shared_ptr` ref-count is `std::atomic` per the C++ standard library.
  Thread-safe for ref-count manipulation across threads; the pointee itself
  is not magically thread-safe.
- Deleter runs on the thread that drops the last `shared_ptr`. That thread
  may differ from the thread that allocated. The destructor must therefore
  be safe to invoke from an arbitrary thread (no thread-affinity assertions
  in `~Session`, etc.).

## Test plan

- Unit: `ObjectPool<TestStruct>::make_shared(args...)` constructs, last
  reference drop invokes destructor, memory returns to pool.
- Unit: `weak_ptr` survives last `shared_ptr` drop, `lock()` returns null.
- Stress: 8 threads, 1M allocations across `Session`-sized, `Packet`-sized,
  and `JobItem`-sized objects.
- TSan: run all unit tests with `-fsanitize=thread`. Must be clean.

## Open questions

1. **Should `make_shared` take an allocator parameter to avoid the
   double-allocation problem?** `std::make_shared` co-locates the control
   block and the object; `shared_ptr(raw, deleter)` does not. We accept the
   second allocation for now because its size is dominated by the control
   block and would land in the smallest bucket anyway.
2. **Intrusive ref-counting.** Reference repo does not use intrusive
   ref-counting; we follow suit. If profiling shows control-block allocation
   is a hot spot, revisit with `boost::intrusive_ptr` or hand-rolled
   equivalent — v2 question.
3. **Pool_alloc ergonomics for std::pmr.** A `std::pmr::memory_resource`
   adapter would let `std::pmr::vector<T>` use the pool without templating
   on `PoolAllocator`. Worth adding once we have a use case.
