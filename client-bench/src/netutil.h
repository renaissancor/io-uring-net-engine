// netutil.h — socket setup and the non-blocking read/write helpers.
//
// Three settings in here had to be right before any measurement was possible,
// and each was invisible when wrong:
//   - SO_RCVBUF must be set BEFORE connect(). Set after, it is cosmetic: the
//     receive window is negotiated during the handshake.
//   - IP_BIND_ADDRESS_NO_PORT is what keeps a bound socket 4-tuple-aware.
//     Without it, binding a source address caps the socket at the ephemeral
//     range per source IP even when destinations differ.
//   - RLIMIT_NOFILE, raised at startup. The 1024 default makes a run die at
//     the 1024th connection and look like a network problem.
#pragma once

#include <netinet/in.h>
#include <sys/resource.h>

#include <cstdint>

#include "config.h"
#include "conn.h"

bool raise_fd_limit(rlim_t want, rlim_t& got);
int  start_connect(const config& cfg, const sockaddr_in& dst, int index, int& out_errno);
bool flush_pending(int fd, conn& c);
bool read_available(int fd, conn& c, uint64_t& bytes_in);

// The receive slab: `slots` x k_rx_cap bytes, one anonymous MAP_NORESERVE
// mapping indexed by fd. Pages are faulted in only where a connection
// actually receives, and consume_frames() keeps the write position near the
// start of the slot, so RSS is roughly one page per live connection rather
// than the slab's virtual size.
bool  rx_slab_init(size_t slots);
char* rx_slab_ptr(int fd);

// Sends one complete frame. With nothing pending it goes straight from the
// caller's buffer to the kernel -- no copy into c.pending, no erase after --
// and only an unsent tail is copied in. With bytes already pending the frame
// is queued behind them and flush_pending() runs, because a direct send would
// overtake the queued tail and desynchronise the stream. Returns false on a
// hard error, exactly like flush_pending().
bool send_frame(int fd, conn& c, const char* frame, size_t len);
