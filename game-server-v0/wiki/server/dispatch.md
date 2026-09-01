# `server/dispatch.{h,cpp}` — `packet_dispatcher`

## Purpose

Maps `packet_id → handler function` at runtime. Populated once at
startup by `iouring_server::generated::register_all()`; read-only
thereafter. Sits between the per-session `recv_packet` loop and the
generated stubs.

## Interface

```cpp
// server/dispatch.h
#pragma once
#include <iouring_net/session.h>
#include <iouring_net/task.h>
#include <array>
#include <span>
#include <cstddef>
#include <cstdint>

class packet_dispatcher {
public:
  // Handler signature carries the id so reject_unknown (and any
  // future metrics handler) can report WHICH id was seen. Real
  // generated stubs ignore the parameter; the cost is one
  // already-in-register argument.
  using handler_fn =
      iouring_net::task<void>(*)(iouring_net::session&,
                                  uint16_t id,
                                  std::span<const std::byte>);

  // Every slot is initialized to reject_unknown — dispatch is always
  // safe to call; unknown IDs close the session via the default.
  packet_dispatcher() noexcept;

  void register_handler(uint16_t id, handler_fn fn);

  // Always defined. Looks up table_[id] (never null — every slot has
  // either a real handler or the reject_unknown default) and awaits.
  iouring_net::task<void>
  dispatch(iouring_net::session& s,
           uint16_t id,
           std::span<const std::byte> body) const;

  // Diagnostic: aggregate count of unknown-ID rejections since
  // construction. Updated by reject_unknown. Not a hot-path field;
  // used by the e2e harness and ops dashboards.
  uint64_t unknown_packet_id_count() const noexcept;

private:
  std::array<handler_fn, 65536> table_;
  mutable std::atomic<uint64_t>  unknown_count_{0};
};
```

`is_registered()` is **not** part of the public surface. Callers do
not pre-check; they call `dispatch()` and let the table drive the
behavior. Eliminating the precheck pattern resolves the
lifecycle/dispatch authority split: there is exactly one place
("which handler runs for ID `i`?") and exactly one way to express
the answer (the table slot).

## Invariants

1. **All `register_handler` calls happen-before any `dispatch`.** This
   is a startup-time pattern: `main` registers, then calls `bind`.
   The dispatcher does not enforce read/write phasing with locks; the
   architecture-level invariant from `01-architecture.md` § "Where
   state lives" is the contract.
2. **Every table slot is non-null at all times.** The constructor
   fills every slot with the `reject_unknown` handler;
   `register_handler` swaps a slot from `reject_unknown` to a real
   stub. `dispatch()` is therefore unconditional — no null check, no
   precheck, no separate "unknown-id" branch in the caller.
3. **Re-registering the same `id` is a hard error.**
   `register_handler` asserts the slot is currently `reject_unknown`
   before writing. Generated `register_all` never repeats an ID; if
   you see this assertion, the generator is buggy or the schema has
   duplicate IDs.
4. **Unknown ID closes the session via the `reject_unknown` slot,
   not via a caller-side branch.** The dispatcher *owns* the
   unknown-id policy. The per-session loop in `lifecycle.cpp` does
   not (and must not) pre-check the registration table — it calls
   `co_await dispatcher_.dispatch(s, id, body)` unconditionally.
5. **The table is 512 KB** (`65536 × 8` on a 64-bit pointer). Paid
   once per process. The hot-path lookup is one indexed load — no
   hash, no `nullptr`-branch, one unconditional indirect call.

## Implementation notes

```cpp
// server/dispatch.cpp
#include "dispatch.h"
#include <cassert>
#include <cstdlib>     // std::abort

namespace {
  // The default-init slot. Defined as a translation-unit-local
  // function so it has stable function-pointer identity, which the
  // re-registration check in register_handler relies on.
  //
  // The dispatcher passes its own `this` through a friend free
  // function or a static accessor so reject_unknown can bump the
  // counter; sketch below uses a local accessor.
  iouring_net::task<void>
  reject_unknown(iouring_net::session& s,
                  uint16_t id,
                  std::span<const std::byte> /*body*/)
  {
    // Bump aggregate counter; the dispatcher exposes a const accessor
    // via an internal friend. See note below the snippet on plumbing.
    packet_dispatcher::tls_for_session(s).bump_unknown();
    s.reject_and_close(iouring_net::close_reason::unknown_packet_id, id);
    co_return;
  }
}

packet_dispatcher::packet_dispatcher() noexcept {
  table_.fill(&reject_unknown);   // every slot starts as the rejector
}

void packet_dispatcher::register_handler(uint16_t id, handler_fn fn) {
  // Null-handler guard. Without this, a generator bug could write a
  // null slot and the next dispatch would crash on indirect call.
  if (fn == nullptr) {
    std::fprintf(stderr,
      "packet_dispatcher::register_handler: null handler for id=%u\n",
      static_cast<unsigned>(id));
    std::abort();
  }

  // Duplicate registration is a hard error in BOTH debug AND release.
  // assert() vanishes in release builds; we must not silently
  // overwrite a previously registered handler, because that would
  // make schema/codegen bugs invisible in production.
  if (table_[id] != &reject_unknown) {
    std::fprintf(stderr,
      "packet_dispatcher::register_handler: duplicate id=%u "
      "(slot is %p, expected reject_unknown)\n",
      static_cast<unsigned>(id),
      reinterpret_cast<void*>(table_[id]));
    std::abort();
  }

  table_[id] = fn;
}

iouring_net::task<void>
packet_dispatcher::dispatch(iouring_net::session& s,
                             uint16_t id,
                             std::span<const std::byte> body) const {
  // Unconditional. Every slot is non-null by construction; if id is
  // unregistered, table_[id] == &reject_unknown which closes the
  // session and bumps unknown_count_. No precheck branch; one
  // indirect call.
  co_await table_[id](s, id, body);
}

uint64_t packet_dispatcher::unknown_packet_id_count() const noexcept {
  return unknown_count_.load(std::memory_order_relaxed);
}
```

**Plumbing note for the counter.** `reject_unknown` is a free function
with no captured state, so it cannot directly reach the dispatcher's
`unknown_count_`. Two reasonable implementation choices, equivalent
for the design:

- **Through-session.** The dispatcher attaches itself to the session
  in `service::adopt_session` or via a `session::dispatcher_hook()`
  setter; `reject_unknown` reaches the dispatcher through `s`. Clean
  but adds a session field.
- **TLS slot.** v1 is single-threaded, so a `thread_local
  packet_dispatcher*` set on entry to `dispatch()` is correct and
  trivially cheap. The snippet above sketches this as
  `packet_dispatcher::tls_for_session(s)` — finalize the exact name
  during implementation.

Pick when the lifecycle is implemented; both are sound and the
choice doesn't affect the dispatcher's public contract.

The `co_await fn(s, body)` chain inlines through the coroutine ABI
into one indirect call + the handler body. No allocation on the
dispatch path itself; whether the handler allocates is its own
business. There is no `if (fn)` guard because **the table cannot
contain null**: every slot is filled with `&reject_unknown` at
construction, and `register_handler` only swaps a slot from
`reject_unknown` to a real handler.

### Why default-rejector over null + caller-precheck

| Concern                                       | Default-rejector slot                                                          | Null + caller precheck                              |
|-----------------------------------------------|--------------------------------------------------------------------------------|-----------------------------------------------------|
| Source-level precheck branches before dispatch| 0 (unconditional indirect call; CPU still pays one BTB lookup for the indirect)| 1 in caller (`if (is_registered(id))`) + 1 in dispatcher (`if (auto fn = ...)`) |
| Authority for "unknown id closes session"     | Dispatcher owns it (one place)                                                 | Split between caller and dispatcher (two places)     |
| Risk of caller forgetting the precheck        | None (no precheck exists)                                                      | Real — earlier draft of `lifecycle.md` skipped it    |
| Code change to add an unknown-id metric       | Update `reject_unknown` (one site)                                             | Add counters in caller AND in dispatcher's else-branch |
| Memory cost                                   | Same 512 KB + 8B counter                                                       | Same 512 KB                                         |
| Codegen impact                                | None (generated `register_all` is unchanged)                                   | None                                                |

The hot-path argument is **branch elimination at the source level**,
not "zero branches at the CPU level." An indirect call through
`table_[id]` is still a control transfer; modern CPUs predict it via
the branch target buffer (BTB). What we save is the data-dependent
*precheck* branch + the null-guard branch — both of which would have
preceded the indirect call. Net: same indirect call, two fewer
predicted-direct branches.

### Why dense table over hash map

| Concern               | `std::array<fn, 65536>` | `std::unordered_map<uint16_t, fn>`       |
|-----------------------|-------------------------|-------------------------------------------|
| Memory                | 512 KB constant         | ~24 bytes × #packets + buckets             |
| Lookup latency        | 1 load                  | hash + probe + load + comparison           |
| Cache behavior        | 1 line per packet       | scattered across buckets                   |
| Thread safety (read)  | trivially fine          | requires `const`-correct map (fine)        |
| Configuration churn   | none                    | none                                       |

For ≤200 packets the maps wins on memory. We have ~15 today and the
ceiling is < 200; 512 KB is a fixed cost the entire process is happy
to pay. The hot-path branch elimination matters more.

(The Windows reference uses an `unordered_map`. We diverge here.)

## Failure routing

There are two protocol-level reject paths that route through the
dispatcher boundary:

| Trigger                                        | Owner                | Action                                                                                   |
|------------------------------------------------|----------------------|------------------------------------------------------------------------------------------|
| `body.size() != X_wire_size` (size mismatch)   | Generated `stub_X`   | `s.reject_and_close(close_reason::bad_payload_size, X_id);` then `co_return`             |
| `id` not registered (no matching stub)         | `reject_unknown` slot| `unknown_count_++`; `s.reject_and_close(close_reason::unknown_packet_id, id);`           |

Both paths call `session::reject_and_close(reason, id)` and let the
session destructor drive the actual fd close on its next dispatch
boundary. The per-session loop in `lifecycle.cpp` observes the
closed session on its next `recv_packet` call and tears down its
coroutine. There is **no third path**: handler-internal failures
(unrecoverable exceptions in user handler code) hit
`std::terminate` per [`01-architecture.md`](../../docs/01-architecture.md)
§ "Failure model" ring 3.

**Dependency on the library.** This design assumes
`session::reject_and_close(close_reason, uint16_t id)` and a
`iouring_net::close_reason` enum (or `framing_error` equivalent) with
at least `bad_payload_size` and `unknown_packet_id` entries. As of
2026-05-14 neither symbol exists in `iouring-net-lib`; landing the
network layer is a prerequisite for this dispatcher's reject paths.
See [`../../../iouring-net-lib/wiki/network/packet_framing.md`](../../../iouring-net-lib/wiki/network/packet_framing.md)
for the framing-layer error catalogue (when it lands).

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/Stub.cpp:54` — dispatch
  via `unordered_map` lookup in the reference.
- The dense-table choice is a Linux-side optimization, not a port.

## Cross-references

- [`lifecycle.md`](lifecycle.md) — the per-session loop that calls
  `dispatch`
- [`handlers.md`](handlers.md) — what `handler_fn` points at
- [`../codegen/pipeline.md`](../codegen/pipeline.md) — generated
  `register_all` that populates the table
- [`../../docs/01-architecture.md`](../../docs/01-architecture.md) §
  "packet_dispatcher"
