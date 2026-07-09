# spsc_mailbox — whole-frame SPSC mailbox for the thread mesh

> **Status:** landed
> **Source:** `src/app/spsc_mailbox.h`
> **Namespace:** `app`
> **Depends:** `message`, `sds::ring_buffer`, `check`, `types`

## Purpose

A thin framing wrapper over `sds::ring_buffer<N, ring_sync::spsc>` that turns a
raw byte SPSC ring into a **whole-message** mailbox for cross-role communication
(acceptor ⇄ worker). One producer thread posts; one consumer thread drains. It
adds length-prefixed framing so a consumer never observes half a message.

## API

```cpp
namespace app {

// Largest body any mesh message may carry. The ring must exceed one whole frame
// (header + this). Current message bodies are ~40 B; raise only if a new body
// approaches this.
inline constexpr usize k_max_msg_body = 240;

template <usize N>
class spsc_mailbox {
public:
    spsc_mailbox() noexcept;
    // non-copyable, non-movable (embeds a ring_buffer)

    // ---- producer side ----
    // Assemble [app_msg_header | body] and enqueue it whole. Returns false and
    // enqueues NOTHING if body_len > k_max_msg_body or the ring lacks room.
    // body may be null iff body_len == 0.
    bool post(app_msg_type type, const void* body, u16 body_len) noexcept;

    // Typed convenience for a POD message struct (sizeof(T) <= k_max_msg_body,
    // enforced by static_assert).
    template <typename T>
    bool post_msg(app_msg_type type, const T& msg) noexcept;

    // ---- consumer side ----
    // Dequeue exactly one whole frame. On success: writes the header to
    // hdr_out, copies the body into body_out (must hold >= header.size bytes),
    // returns true. Returns false and consumes NOTHING when no complete frame
    // is available.
    bool try_recv(app_msg_header& hdr_out, byte* body_out, usize body_cap) noexcept;

    // ---- observers ----
    bool  empty()      const noexcept;
    usize used_bytes() const noexcept;   // advisory under concurrency
    usize free_bytes() const noexcept;   // advisory under concurrency
    static constexpr usize capacity() noexcept;   // == N
};

// Concrete mesh edges.
using acceptor_to_worker_mailbox = spsc_mailbox<64 * 1024>;  // hot admission path
using worker_to_acceptor_mailbox = spsc_mailbox<16 * 1024>;  // close-notify path

}  // namespace app
```

Wire frame in the ring: `[ app_msg_header | body ]`, where `header.size` = body
byte count (excludes the header) and `header.type` = `app_msg_type`.

## Invariants

- **SPSC contract:** at most one thread posts, at most one thread drains. No
  runtime guard — violating it is UB (the name is the warning).
- **Whole-frame post:** `post` enqueues the entire frame or nothing (the ring's
  `enqueue` is all-or-nothing).
- **Whole-frame recv:** `try_recv` verifies the ENTIRE frame is present (peek
  header → confirm `header + body` bytes available) before consuming a byte, so
  a partial frame is never stranded. (Given all-or-nothing post a partial can't
  arise today; the check keeps the parser independent of writer behavior.)
- **Bounded:** overflow is backpressure (`post` returns false), never a resize.
- **Sizing:** `N > sizeof(app_msg_header) + k_max_msg_body` — enforced by
  `static_assert`.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `post` body_len > `k_max_msg_body` | returns `false`, enqueues nothing |
| `post` ring lacks room for whole frame | returns `false`, enqueues nothing |
| `post_msg` `sizeof(T)` > `k_max_msg_body` | compile error (`static_assert`) |
| `try_recv` fewer than `sizeof(header)` bytes buffered | returns `false`, consumes nothing |
| `try_recv` header present but body incomplete | returns `false`, consumes nothing |
| framed `header.size` > `k_max_msg_body` or > `body_cap` | `LNX_CHECK` trap (framing/producer bug, not a soft error) |

## Notes

- Non-movable: mailboxes are LANDLORD-owned — the supervisor constructs them as
  named locals / pinned storage that outlive every thread; handles borrow raw
  pointers via `install_mailboxes()`.
- `post` assembles the frame in an on-stack `byte[sizeof(header)+k_max_msg_body]`
  scratch, then one `enqueue`. `try_recv` peeks the 4-byte header without
  advancing, then consumes header + body only after the full-frame check.
- Body is copied out into a caller buffer (memcpy into a struct), so consumers
  cast an aligned local, never the ring interior.

## Test plan

`tests/app/spsc_mailbox_test.cpp`:
- empty mailbox → `try_recv` false, consumes nothing
- typed post/recv round-trips header + body
- FIFO order preserved across mixed message types
- body > `k_max_msg_body` → `post` refused
- ring full → `post` fails cleanly; drain one → `post` succeeds again

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan
- [x] This spec matches the built API
- [x] Aliases wired into `handle_acceptor` / `handle_worker` via `install_mailboxes`

## Rationale

- `.omc/wiki/inter-thread-comms-spsc-mesh-pattern.md` — SPSC-everywhere mesh decision
- `design/` — thread-mesh message framing (peek-verify-dequeue) discussion
- `handoff.md` § "New `src/app/spsc_mailbox.h`"
