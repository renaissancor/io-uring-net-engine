# message — POD thread-mesh message vocabulary

> **Status:** landed
> **Source:** `src/message.h`
> **Namespace:** `app`
> **Depends:** `session_id`, `types`

## Purpose

The set of structs that cross the thread mesh between acceptor and worker, plus
the discriminant enum and the 4-byte length-prefix header that `mesh.h` frames
them with. Every message is a trivially-copyable POD so the pipe can move it
with a plain `memcpy`.

## API

```cpp
namespace app {

enum class app_msg_type : u16 {
    adopt_session  = 1,   // acceptor -> worker: take ownership of an accepted fd
    session_closed = 2,   // worker -> acceptor: fd owner released the session
    stop           = 3,   // supervisor/peer -> role: cooperative drain request
};

// 4-byte length-prefix header. `size` is the body length so the ring transport
// stays type-agnostic; `type` selects the body struct at the consumer.
struct app_msg_header {
    u16 size;   // body bytes following this header
    u16 type;   // app_msg_type
};

// acceptor -> worker. The acceptor accepts the fd, mints id+generation+fake
// account, then immediately hands the fd to a worker which becomes its SOLE
// owner (invariant: one fd, one owner thread). initial_room is k_no_room on the
// v1 immediate-adopt path.
struct adopt_session_msg {
    int        fd;
    session_id id;
    u32        generation;
    account_id account;
    room_id    initial_room;
};

// worker -> acceptor. The worker released the fd (client quit, error, or
// drain). Carries generation so the acceptor can discard a close that refers to
// an id/slot that has already been recycled.
struct session_closed_msg {
    session_id id;
    u32        generation;
    worker_id  worker;
    u32        reason;
};

}  // namespace app
```

Frame in the pipe: `[ app_msg_header | body bytes ]`. `header.size` counts
body bytes only, never the header. `header.type` is the `app_msg_type` value
cast to `u16`.

## Invariants

- **Blittable:** every struct here has only integer members and no
  constructors, destructors, or virtuals. `mesh_post_msg` copies
  `sizeof(T)` bytes of the object as the frame body; the consumer `memcpy`s them
  back into an aligned local.
- **Header is the only framing:** `app_msg_header` is two `u16`, 4 bytes. There
  is no magic, checksum, or version field.
- **`stop` has no body struct:** it is posted with `body_len == 0` via
  `mesh_post` and arrives as a header-only frame.
- **Generation travels with the id:** both message bodies carry `generation`
  so a receiver holding `session_table` can call `validate(id, generation)`
  and drop frames that refer to a recycled slot.
- **Layout is the compiler's natural layout:** no packing attribute is applied;
  structs contain padding (`adopt_session_msg` is 40 bytes, `session_closed_msg`
  24 bytes on x86-64). Producer and consumer are the same binary, so this is
  never a portability concern, but padding bytes are copied uninitialised
  unless the sender value-initialises (`msg{}`).

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `header.type` value not in `app_msg_type` | nothing in this header rejects it; the consumer's dispatch decides |
| `header.size` disagrees with `sizeof` of the struct named by `type` | not checked here; `mesh_try_recv` only checks `size <= body_cap` |
| Body larger than `k_mesh_body_max` (1024) | compile error from `mesh_post_msg`'s `static_assert`, not from this header |

## Notes

- `session_closed_msg` is the sizing input for the `static_assert` in `mesh.h`
  that requires `config::k_session_capacity` close frames to fit the
  `worker_to_acceptor_pipe`.
- `app_msg_header::type` is a raw `u16`, not `app_msg_type`, so readers compare
  against `static_cast<u16>(app_msg_type::...)`.
- The `adopt_session_msg` comment mentions worker-side room selection; that path
  is not built.

## Test plan

No dedicated test. Every case in `tests/mesh_test.cpp` uses these types as the
framed payload:
- post/recv round-trips a typed message — `adopt_session_msg` body, checks
  `hdr.type` and `hdr.size == sizeof(adopt_session_msg)`
- FIFO order across mixed message types — `adopt_session_msg` then `session_closed_msg`
- zero-length body round-trips — `app_msg_type::stop` header-only frame
- an incomplete frame consumes nothing — hand-built `app_msg_header{64, adopt_session}`
- post fails cleanly when the pipe is full — repeated `adopt_session_msg`
- frames straddling the pipe wrap round-trip intact — `adopt_session` type tag
- two frames in flight across the wrap keep FIFO order — both type tags
- enqueue2 frames survive a real producer thread intact — alternates both type tags
