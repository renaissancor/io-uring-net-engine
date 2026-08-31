# mesh — length-prefixed framing over a thread-mesh byte pipe

> **Status:** landed
> **Source:** `src/app/mesh.h`
> **Namespace:** `app`
> **Depends:** `message`, `sds::pipe`, `check`, `types`

## Purpose

Framing for cross-role communication (acceptor ⇄ worker) over a raw
`sds::pipe<N>`. The pipe moves bytes with no message boundaries — exactly like
TCP — and this layer puts `[header|body]` frames on top so a reader never
observes half a message.

Deliberately **free functions, not a wrapper class**: the pipe *is* the mesh
edge. Wrapping it would hide the byte API that the socket path also parses
against, and the goal is ONE "read length-prefixed frames off a byte stream"
loop serving socket bytes and peer-thread bytes alike.

Supersedes the retired `app::spsc_mailbox`, which staged each frame in an
on-stack scratch buffer (two copies per message) and capped bodies at 240 B.

## API

```cpp
namespace app {

// Largest body in the mesh VOCABULARY — used to size receive buffers. This
// bounds the protocol, not the transport: a frame is limited only by the
// pipe's capacity, same as a pipe(2).
inline constexpr usize k_mesh_body_max = 1024;

// Bytes a frame occupies for a body of `body_len`.
constexpr usize mesh_frame_size(usize body_len) noexcept;

// ---- producer side ----
// Post a raw body under `type` as ONE atomic [header|body] frame, via
// pipe::enqueue2 — no staging buffer. Returns false, writing NOTHING, when the
// pipe lacks room for the whole frame. body may be null iff body_len == 0.
template <typename Pipe>
bool mesh_post(Pipe& p, app_msg_type type, const void* body, u16 body_len) noexcept;

// Typed convenience for a POD message struct from message.h
// (sizeof(T) <= k_mesh_body_max, enforced by static_assert).
template <typename Pipe, typename T>
bool mesh_post_msg(Pipe& p, app_msg_type type, const T& msg) noexcept;

// ---- consumer side ----
// Read exactly one whole frame. On success: writes the header to hdr_out,
// copies the body into body_out (must hold >= header.size bytes), returns true.
// Returns false and consumes NOTHING when no complete frame is ready.
template <typename Pipe>
bool mesh_try_recv(Pipe& p, app_msg_header& hdr_out,
                   byte* body_out, usize body_cap) noexcept;

// Concrete mesh edges.
using acceptor_to_worker_pipe = sds::pipe<64 * 1024>;  // hot admission path
using worker_to_acceptor_pipe = sds::pipe<16 * 1024>;  // close-notify path

}  // namespace app
```

Frame in the pipe: `[ app_msg_header | body ]`, where `header.size` = body byte
count (excludes the header) and `header.type` = `app_msg_type`.

## Invariants

- **SPSC contract:** at most one thread writes, at most one reads. No runtime
  guard — violating it is UB (inherited from `sds::pipe`).
- **Atomic publish:** `mesh_post` writes header and body through
  `pipe::enqueue2`, which advances the producer cursor **once**, after both
  regions land. A partial frame is never observable.
- **Whole-frame recv:** `mesh_try_recv` verifies the ENTIRE frame is present
  (peek header → confirm `header + body` bytes available) before consuming a
  byte, so the reader never depends on writer behavior it cannot see.
- **Bounded:** overflow is backpressure (`mesh_post` returns false), never a
  resize.
- **No transport size cap:** unlike the retired mailbox, framing imposes no
  body limit; the pipe's capacity is the only bound.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `mesh_post` pipe lacks room for whole frame | returns `false`, writes nothing |
| `mesh_post` `body_len == 0` with null body | legal — header-only frame |
| `mesh_post_msg` `sizeof(T)` > `k_mesh_body_max` | compile error (`static_assert`) |
| `mesh_try_recv` fewer than `sizeof(header)` bytes buffered | returns `false`, consumes nothing |
| `mesh_try_recv` header present but body incomplete | returns `false`, consumes nothing |
| framed `header.size` > `body_cap` | `LNX_CHECK` trap (producer/consumer sizing bug, not a soft error) |

## Notes

- Pipes are LANDLORD-owned — the supervisor constructs them as named locals
  that outlive every thread; ctls borrow raw pointers via `install_pipes()`.
- `mesh_try_recv` copies the body into a caller buffer, so consumers memcpy into
  an aligned local and never cast the pipe interior.
- Frames straddling the pipe's wrap point are handled by `enqueue2`'s
  two-region write and `dequeue`'s reassembly; both are covered by tests.

## Test plan

`tests/app/mesh_test.cpp`:
- empty pipe → `mesh_try_recv` false, consumes nothing
- typed post/recv round-trips header + body
- FIFO order preserved across mixed message types
- zero-length body round-trips
- header present but body incomplete → false, consumes nothing; completes later
- pipe full → `mesh_post` fails cleanly; drain one → succeeds again
- frames straddling the wrap round-trip intact (single and two-in-flight)

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan
- [x] This spec matches the built API
- [x] Aliases wired into `acceptor_ctl` / `worker_ctl` via `install_pipes`

## Rationale

- `.omc/wiki/inter-thread-comms-spsc-mesh-pattern.md` — SPSC-everywhere mesh decision
- `doc/sds/pipe.md` — why the mesh edge is named separately from `ring_buffer`
- Mailbox retirement: the wrapper cost two copies per message and an artificial
  240 B body cap, and forced a second parse loop distinct from the socket path.
