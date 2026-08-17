// netutil.h — socket setup and the non-blocking read/write helpers.
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
