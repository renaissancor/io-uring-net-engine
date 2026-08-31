#include "wire.h"

#include <cstring>

proto g_proto;

void put_frame(std::string& out, uint16_t type, const char* data, size_t len)
{
    const auto n = static_cast<uint16_t>(
        g_proto.len_includes_header ? len + k_header_size : len);
    char hdr[k_header_size];
    std::memcpy(hdr,     &n,    sizeof(n));
    std::memcpy(hdr + 2, &type, sizeof(type));
    out.append(hdr, sizeof(hdr));
    out.append(data, len);
}

void put_frame(std::string& out, uint16_t type, const std::string& payload)
{
    put_frame(out, type, payload.data(), payload.size());
}

// Payload byte count for a header whose length field read as `raw`. Returns
// false when the frame is malformed, which under the inclusive convention
// includes any size below the header itself.
bool payload_len(uint16_t raw, size_t& out)
{
    if (!g_proto.len_includes_header) { out = raw; return true; }
    if (raw < k_header_size) return false;
    out = static_cast<size_t>(raw) - k_header_size;
    return true;
}
