#pragma once
// server/seam_check.h
//
// v0 install-contract probe: round-trips bytes through the installed
// library's sds::ring_buffer, proving headers resolve from the install
// prefix and symbols link from libiouring_net.a. Retired once real
// runtime code exercises the seam on every path.

namespace srv {

bool seam_self_check();

}  // namespace srv
