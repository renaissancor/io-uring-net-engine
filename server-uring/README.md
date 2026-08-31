# server-uring — reserved, not yet written

The chat/game server built **on top of** `engine-uring`. Nothing here yet.

This directory exists so the boundary is visible before the code is: the
product consumes the engine through `find_package(iouring_net)` against an
install prefix, and never through a relative include into `../engine-uring/`.
That rule is the whole reason the seam is worth having, and it is the one thing
a monorepo makes easy to break by accident.

## What has to be true when it lands

- Its own `CMakeLists.txt`, configured independently. **No top-level
  `CMakeLists.txt` ties the two together** — the moment one exists,
  `add_subdirectory(../engine-uring)` becomes the path of least resistance and
  the install/export contract stops being tested by anything.
- The build is: install the engine to a prefix, then configure this against
  that prefix. If that round trip is not exercised, the engine's public header
  set is a claim rather than a fact.
- Its own CI lane. The engine wants TSan/ASan-clean unit and property tests;
  the product wants integration tests and traffic replay. Different cadences,
  different definitions of green.

## What the design docs say, and how much of it survives

[`../design-notes/09-project-split.md`](../design-notes/09-project-split.md)
specifies the boundary criteria, what belongs on each side, and the contract
surface. Read it for the boundary. Ignore its repo topology: it argued for a
separate repository, that argument was rejected, and the banner at the top of
it says so.

An earlier scaffold for this component was written and then discarded. One
thing in it was worth keeping and is recorded here rather than lost: a probe
that round-trips bytes through the *installed* `sds::ring_buffer` makes the
seam fail loudly at test time instead of silently resolving headers from the
source tree. Whatever lands here should do that on day one.
