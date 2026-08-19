// corpus.h — realistic chat text, built once, indexed in the hot loop.
//
// The original constraint stands: no randomness inside the send loop, because
// generating text there burns client CPU and client CPU lands in the histogram
// as server latency. This does not relax that constraint, it front-loads it.
// Every line is assembled at startup from a fixed seed into a flat vector, and
// the hot loop does one index and returns a reference — the same cost as the
// fixed-string classes it replaces.
//
// The seed is fixed rather than time-based so two runs, and two nodes of one
// fleet, send byte-identical traffic. A load generator that is not
// reproducible cannot support a claim about a change.
//
// Lengths follow how chat actually distributes: overwhelmingly short, with a
// thin tail of paragraphs. That matters because frame size decides how much
// work each delivery is, and a uniform 64 bytes exercises one point on that
// curve and calls it the answer.
//
// It does NOT decide segmentation. The study server caps a payload at 1024
// bytes, so k_max_blob leaves the longest line at 988 — under the 1448-byte
// MSS, meaning no single chat frame is ever segmented. What crosses the MSS is
// the server's batched flush, where many frames coalesce into one send(). An
// earlier version of this comment claimed the corpus reached the segmentation
// path on its own; it cannot, and the distinction matters when reading which
// mode a measurement is exercising.
//
// Text is Korean, so it is also UTF-8 multi-byte: one character is three
// bytes. That is deliberate. The protocol's invariant is the 1 KB byte cap and
// the character limit is derived from it, never the other way round, and a
// corpus of ASCII would let that distinction go untested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class corpus {
public:
    // entries is how many lines to materialise, sampled from the pool in
    // corpus_data.cpp. Pool size and entries are separate knobs on purpose:
    // the pool is .rodata and costs binary bytes, entries is resident memory
    // the send path walks, and a few thousand entries keeps that walk in cache.
    //
    // entries exceeding the pool size does not mean a run repeats itself in
    // any way that matters. At 4096 entries against the current 3323-line pool
    // the build is 59% distinct, and the repetition is concentrated in the
    // reaction class by design: 250 tiny lines serve 45% of draws, because a
    // real room really does say the same six things all day. The classes that
    // carry bytes barely repeat -- mid draws ~985 lines from 876, long draws
    // ~29 from 150. Raise entries if you want a longer non-repeating stretch;
    // the cost is resident memory, not fidelity.
    void build(uint32_t seed, size_t entries, size_t max_bytes);

    // O(1), no allocation, no branching on content.
    const std::string& next()
    {
        const std::string& s = lines_[cursor_];
        if (++cursor_ == lines_.size()) cursor_ = 0;
        return s;
    }

    size_t size()      const { return lines_.size(); }
    size_t total_bytes() const { return total_; }
    size_t min_bytes() const { return min_; }
    size_t max_bytes() const { return max_; }
    double mean_bytes() const
    {
        return lines_.empty() ? 0.0
             : static_cast<double>(total_) / static_cast<double>(lines_.size());
    }

private:
    std::vector<std::string> lines_;
    size_t cursor_ = 0;
    size_t total_  = 0;
    size_t min_    = 0;
    size_t max_    = 0;
};
