# SerialBuffer — fixed-size scratchpad for packet serialization

## Purpose

A fixed-size byte buffer (default 4 KiB) used to assemble a single outbound
packet before it is copied into `SendRingBuffer`. Distinct from
`RingBuffer` — `SerialBuffer` is contiguous, has no wrap, and is meant to
live on the stack of a packet-build function (or as a `thread_local`
scratch).

The reference implementation uses this for safe write-with-bounds-check of
the `[uint16 size | uint16 id][payload]` framing header where `size` is
back-patched after the payload is fully written.

## Reference origin

- `WindowsLibrary/Library/Include/SerialBuffer.h:5` — fixed `4096`-byte
  buffer with cursor-based write API.

## Public API sketch

```cpp
namespace iouring_net::buf {

template <size_t N = 4096>
class SerialBuffer {
public:
    SerialBuffer() = default;

    // Reserve N bytes at the head; returns pointer the caller writes to
    // and remembers for later back-patching. Advances cursor by N.
    template <class T>
    T* reserve();                                 // T must be trivially copyable

    template <class T>
    void write(const T& v);                       // memcpy-append

    void write_bytes(std::span<const std::byte> b);

    // Read-only cursor / size accessors
    const std::byte* data() const;
    size_t           size() const;
    size_t           remaining() const;
    bool             can_fit(size_t bytes) const;

    void clear();

private:
    std::array<std::byte, N> storage_;
    size_t                   cursor_{0};
};

} // namespace iouring_net::buf
```

Typical use:

```cpp
SerialBuffer<> buf;
auto* hdr = buf.reserve<PacketHeader>();          // remember location
write_payload(buf);                               // appends body
hdr->size = static_cast<uint16_t>(buf.size());    // back-patch length
hdr->id   = static_cast<uint16_t>(PacketId::S_HELLO);
session.send(buf.data(), buf.size());             // copy into send ring
```

## Linux design

Pure data structure — no system calls, no synchronization. Same on Linux as
on Windows.

**Bounds checking.** Every write checks `can_fit`. Failure returns
`expected<void, std::errc::value_too_large>` from a future `try_write`
variant; the simple `write` path asserts in debug, truncates in release
(decision pending — see open questions).

**Default size.** 4096 bytes — large enough for any v1 packet. The
framing-header size field is `uint16_t`, so logical packet ceiling is
65535 bytes. v1 caps at 4 KiB and rejects larger writes; v2 may move
oversized packets to a heap-allocated `std::vector<std::byte>`.

**Alignment.** `std::array<std::byte, N>` has natural alignment; for
typed `reserve<T>` the cursor is aligned up to `alignof(T)` before the
return. Misalignment of `T` inside the buffer is therefore impossible.

## Concurrency & ownership

- Stack-local or `thread_local`. Never shared across threads. No
  synchronization.
- Lifetime is the duration of one packet build. Cleared (`clear()`) at the
  start of each build; storage is reused.

## Test plan

- Unit: write each primitive type, assert byte-exact layout.
- Unit: `reserve<Header>` + payload write + back-patch sequence; assert
  `data()` matches the expected wire bytes.
- Unit: overflow path — fill to capacity, attempt one more byte, assert
  the chosen failure mode (assert / `expected` / truncate).
- Fuzz: random write sequences of mixed types up to capacity; check
  `size() == sum(write_n)` and no out-of-bounds writes.

## Open questions

1. **Failure mode on overflow.** Three options:
   - **Assert in debug, undefined in release.** Current reference behavior.
   - **`expected<void, std::errc>` return.** Forces every call site
     to check.
   - **Truncate silently.** Bad — masks bugs.
   Recommendation: `try_write` returns `expected`; `write` asserts.
   Two-API surface, caller picks based on context.
2. **Static N vs. runtime N.** Reference is fixed at 4096. We make it a
   template parameter so `SerialBuffer<512>` is a valid stack-friendly
   variant. No runtime-sized version yet.
3. **Endianness.** Wire format is little-endian (Windows-native). Linux
   x86-64 is also little-endian, so naive `memcpy` is correct. ARM64
   little-endian is the same. **Document explicitly: do not change.** If we
   ever target a big-endian platform, every `write<T>` becomes a byte-swap.
