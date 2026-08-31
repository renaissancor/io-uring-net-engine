#pragma once
// sds/pipe.h
//
// A bounded byte stream between exactly two threads — one writer, one reader.
// Semantically a pipe(2): no message boundaries, no growth, and a full pipe
// refuses the write rather than blocking or resizing.
//
// This is the SAME mechanism as sds::ring_buffer, named for its other role.
// The distinction is which side of the engine you are on:
//
//   sds::ring_buffer<N, ring_sync::single>   socket I/O. io_uring fills the
//                                            enqueue region; the owning worker
//                                            parses frames off the dequeue
//                                            region. One thread, plain cursors.
//
//   sds::pipe<N>                             thread mesh. A peer writes frames
//                                            in, the owner reads them out.
//                                            Two threads, atomic64 cursors with
//                                            acquire/release publication.
//
// Keeping "ring_buffer" for the socket side and "pipe" for the mesh side means
// a use site never has to be read twice to learn which traffic it carries. The
// byte API is identical across both, so ONE "read length-prefixed frames off a
// byte stream" loop serves socket bytes and peer-thread bytes alike — write
// that loop as a template over the byte API, not against a concrete type.
//
// Framing is NOT this layer's job, exactly as it is not TCP's. A pipe moves
// bytes; app/mesh.h puts [header|body] frames on top. Use enqueue2() to publish
// a header and body as one atomic frame with no staging buffer.
//
// Contract: AT MOST one thread writes, AT MOST one thread reads. Violating it
// is UB — there is no runtime guard.

#include "ring_buffer.h"
#include "../types.h"

namespace sds {

template <usize N>
using pipe = ring_buffer<N, ring_sync::spsc>;

}  // namespace sds
