// connect.h — the establish phase.
//
// Fails on: ephemeral port exhaustion (EADDRNOTAVAIL at 28,232 per source IP),
// RLIMIT_NOFILE, listen backlog, and O(N^2) join notices when rooms are too
// large. None of those can happen during the traffic phase, and none of the
// traffic phase's failures can happen here.
//
// Separated from the traffic phase because
// the two fail for entirely different reasons: this one runs into ephemeral
// ports, fd limits and listen backlogs, and it is finished before a single
// latency sample exists.
#pragma once

#include <netinet/in.h>

#include <vector>

#include "config.h"
#include "conn.h"

// Returns the number of connections established, or -1 on a hard failure.
// Fills `live` with the ready file descriptors.
int run_connect(int ep, const config& cfg, const sockaddr_in& dst,
                std::vector<conn>& conns, std::vector<int>& live);
