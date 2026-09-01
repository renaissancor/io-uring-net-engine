// server/seam_check.cpp

#include "seam_check.h"

#include <cstring>

#include <sds/ring_buffer.h>
#include <types.h>

namespace srv {

bool seam_self_check() {
    constexpr char msg[] = "iouring_net seam ok";
    constexpr usize len  = sizeof(msg);

    sds::ring_buffer rb(64);
    if (rb.enqueue(msg, len) != len) return false;

    char out[len] = {};
    if (rb.dequeue(out, len) != len) return false;
    return std::memcmp(msg, out, len) == 0;
}

}  // namespace srv
