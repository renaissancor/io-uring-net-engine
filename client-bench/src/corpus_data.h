// corpus_data.h — the hand-authored message pool, declared for corpus.cpp.
//
// Split out from corpus.cpp because the pool is large and would otherwise be
// re-parsed by anything that touches the corpus. Buckets are by byte length:
// that is the property the corpus exists to model, since length decides which
// path a payload takes through the stack.
#pragma once

#include <cstddef>

namespace corpus_data {

// Reactions and single-word replies. Dominates by count in real logs,
// contributes almost nothing by volume.
extern const char* const k_tiny[];
extern const size_t      k_tiny_n;

// One casual sentence.
extern const char* const k_short[];
extern const size_t      k_short_n;

// One or two sentences — the length someone types when explaining something.
extern const char* const k_mid[];
extern const size_t      k_mid_n;

// The paragraph tail. Rare by count, and the only class that puts a large
// single frame on the wire.
extern const char* const k_long[];
extern const size_t      k_long_n;

// Paragraphs sitting just under the protocol's payload ceiling. The study
// server silently drops a broadcast over 1024 bytes, so this bucket is the
// one that walks up to that edge on purpose rather than by accident.
extern const char* const k_xlong[];
extern const size_t      k_xlong_n;

}  // namespace corpus_data
