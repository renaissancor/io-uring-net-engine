# PacketHandler — id-keyed dispatch

## Purpose

Dispatch incoming packets to user-provided handler functions keyed on the
packet `id`. Handlers are coroutines (`task<void>`) so they can await
further I/O, post jobs to a job queue, or hop threads. The handler table
is constructed at startup; the dispatch is a flat-array lookup.

## Reference origin

The Rookiss lecture introduces a code-generated handler table per
protocol descriptor file. **`SelectServer/TestSerialize/` ships a working
version of this:**

```
TestSerialize/
├── packets.json     ← schema: id, name, fields per packet
├── rpc_gen.py       ← orchestrator (pre-build step)
├── stub_gen.py      ← generates server-side ProcessPacket() switch
└── proxy_gen.py     ← generates client-side typed send wrappers
```

Run as a Visual Studio pre-build step; output is checked-in C++ that
defines per-id codecs and a dispatch switch. The other repos use inline
switches (`SelectServer/FighterOOP/Net.cpp:5-72`) instead.

For v1 of this library we hand-write codecs (the schema is small) and
defer the codegen story to a follow-up. When that follow-up arrives,
`TestSerialize/`'s `rpc_gen.py` is the natural starting point.

## Public API sketch

```cpp
namespace iouring_net::net {

using handler_fn = std::function<
    iouring_net::rt::task<void>(session&, frame_view)>;

class packet_handler_table {
public:
    void register_handler(uint16_t id, handler_fn fn);

    iouring_net::rt::task<void> dispatch(session& s, frame_view f) const;

    bool has_handler(uint16_t id) const;

private:
    std::array<handler_fn, 65536> table_;        // id → fn
    std::function<iouring_net::rt::task<void>(session&, frame_view)>
        unhandled_default_;
};

template <class Pkt>                              // Pkt: trivially copyable
struct packet_codec {
    static std::optional<Pkt> decode(frame_view f) noexcept;
    static void encode(const Pkt& p, packet_writer& w);
};

} // namespace iouring_net::net
```

User code:

```cpp
struct CS_HELLO { uint32_t player_id; char nickname[32]; };
constexpr uint16_t CS_HELLO_ID = 1;

table.register_handler(CS_HELLO_ID,
    [](session& s, frame_view f) -> task<void> {
        auto pkt = packet_codec<CS_HELLO>::decode(f);
        if (!pkt) co_return;                      // bad packet
        co_await s.send(make_response(*pkt));
    });
```

## Linux design

**Table structure.** Flat array of size 65536 (one slot per id). Memory
cost: ~1 MiB per process (16 bytes per `std::function`, 65536 entries).
Acceptable for a process; a sparse hash map would save memory but cost a
hash on every dispatch. Decision: flat array.

**Dispatch.**

```cpp
task<void> packet_handler_table::dispatch(session& s, frame_view f) const {
    if (auto& fn = table_[f.id]; fn) {
        co_await fn(s, f);
    } else if (unhandled_default_) {
        co_await unhandled_default_(s, f);
    } else {
        // Log and disconnect on unknown id. Configurable.
    }
}
```

**Codec.** `packet_codec<Pkt>::decode` performs:
1. `f.payload.size() == sizeof(Pkt)` check (allow `<=` for forward-compat?
   v1: strict equality; v2: trailing-bytes-allowed).
2. `memcpy` from payload bytes into a stack-local `Pkt`.

`packet_codec<Pkt>::encode` writes via `packet_writer`. Both are free
functions; specializations live with the protocol headers.

**Codegen — punt.** A future tool could parse a `.proto`-shaped DSL and
generate `packet_codec<Pkt>` specializations + handler stubs. Out of
scope for v1; we hand-write codecs for the small set of test packets.

**Strict trivially-copyable.** Generic `Pkt` decode requires
`std::is_trivially_copyable_v<Pkt>` and `alignof(Pkt) <= 4` (so no
alignment-violating reinterpret_cast on the wire bytes). Variable-length
fields require a hand-written codec (e.g., string fields in `CS_CHAT`).

## Concurrency & ownership

- The handler table is constructed once at startup, then read-only for
  the rest of the process. No synchronization on dispatch.
- A handler runs on the session's reactor thread by default. Hopping to
  a worker / job queue is the handler's responsibility (`co_await
  job_queue::co_push(...)`).
- The handler captures must outlive any awaitables they create. Lambda
  captures by `shared_ptr` are the norm.

## Test plan

- Unit: register handler for id 5, dispatch a frame with id 5, assert
  handler invoked exactly once with correct payload bytes.
- Unit: dispatch unknown id with no default → logs and disconnects.
- Unit: dispatch unknown id with default → default handler runs.
- Unit: codec for a 24-byte struct round-trips bytewise.
- Unit: codec rejects payload size mismatch.
- Stress: 1M packets across 100 ids dispatched through one table; assert
  no drops or misroutes.

## Open questions

1. **Flat array vs. dense hash.** Flat array is the simplest and fastest
   for our scale. Reconsider if a deployment cares about RSS over CPU.
2. **Versioning.** Adding a new packet id is additive (new slot in the
   table). Removing one is breaking. Document in the protocol changelog;
   no in-code support for "legacy id behavior".
3. **Variable-length packets.** v1 hand-writes their codecs. v2 may
   adopt a small DSL or a shape like Cap'n Proto. Out of scope.
4. **Handler exception policy.** A handler that throws — disconnect the
   session, or log and continue? **Decision: disconnect.** Bugs in
   handlers should not silently desync the protocol.
5. **Async vs. sync handlers.** All handlers are `task<void>`. A purely
   synchronous handler still returns `task<void>` and `co_return`s —
   one-line cost for uniform dispatch. We do not add a parallel sync
   path.
