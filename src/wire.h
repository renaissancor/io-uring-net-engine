// wire.h — frame layout shared by both target servers.
//
// Owns: the 4-byte header, the study/iouring length-convention seam, and the
// 20-byte measurement blob carried inside a chat payload.
//
// Two things here fail silently rather than loudly, which is why they are in
// one file and commented at length below:
//   - --proto backwards desynchronises the stream by four bytes per frame.
//     Every subsequent parse is garbage and nothing reports an error.
//   - the blob's node stamp is the only thing keeping another loadgen
//     process's clock out of this process's histogram.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ------------------------------------------------------------------- wire
//
// Both targets use the same 4-byte little-endian header with no byte
// swapping, and they differ in exactly one respect:
//
//   epoll-chat-study   [uint16 len ][uint16 type]   len  = payload bytes
//   iouring-net-*      [uint16 size][uint16 id  ]   size = payload + header
//
// The width is identical; what differs is whether the length field counts the
// header. That is the entire porting seam, so it is the one thing --proto
// switches. Getting it backwards does not fail loudly — it desynchronises the
// stream by four bytes per frame and every subsequent parse is garbage — so
// it is worth having as an explicit flag rather than an edit.

constexpr size_t k_header_size = 4;   // uint16 + uint16, both targets

struct proto {
    bool     len_includes_header = false;  // false: study, true: iouring-net
    uint16_t id_set_nick         = 1;
    uint16_t id_join             = 2;
    uint16_t id_chat             = 3;
    uint16_t id_notice           = 100;
    uint16_t id_chat_out         = 101;
};

// The IDs above are epoll-chat-study's. The iouring-net product assigns its
// own from a schema (see iouring-net-server/docs/04-protocol.md § packet ID
// ranges) and that schema does not exist yet, so --proto currently switches
// framing only. Fill these in from the generated table when there is one
// rather than guessing: an unrecognised ID closes the session there, so a
// wrong guess presents as a connection failure and not as a protocol error.
extern proto g_proto;

// The study server caps a payload at 1024 and silently drops anything larger.
// What it broadcasts is "nick: " + our blob, so our own budget is smaller
// than 1024 by the length of that prefix. Leave room for a 6-digit nick.
constexpr size_t k_max_payload  = 1024;
constexpr size_t k_prefix_slack = 16;
constexpr size_t k_max_blob     = k_max_payload - k_prefix_slack;

// Our chat payload. The timestamp is the INTENDED send time, not the actual
// one — see the scheduling comment in run_traffic().
//
//   [8B intended_ts_ns][4B seq][4B node_id][4B client_id][filler ...]
//
// node_id is the ownership stamp and it is not decoration. A room can contain
// clients belonging to a different loadgen process, and the receive path used
// to subtract whatever timestamp arrived without asking whose it was. On one
// machine that is accidentally harmless — CLOCK_MONOTONIC is shared — but
// across machines each clock counts from its own boot, so a foreign sample is
// off by however long the two boxes differ in uptime. Hours of negative
// latency, silently averaged into the histogram. Sample only our own.
constexpr size_t k_blob_header = 20;


void put_frame(std::string& out, uint16_t type, const char* data, size_t len);
void put_frame(std::string& out, uint16_t type, const std::string& payload);
bool payload_len(uint16_t raw, size_t& out);
