#include "corpus.h"
#include "corpus_data.h"

#include <algorithm>

namespace {

// A tiny deterministic PRNG rather than <random>. std::mt19937 would work
// equally well; what matters is that the sequence is fixed by the seed and
// identical on every machine in the fleet, which the standard distributions do
// not guarantee across implementations.
struct lcg {
    uint64_t s;
    explicit lcg(uint32_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(s >> 33);
    }
    size_t pick(size_t n) { return next() % n; }
    // Percent roll, 0..99.
    uint32_t roll() { return next() % 100; }
};

const char* pick_from(lcg& r, const char* const* arr, size_t n)
{
    return arr[r.pick(n)];
}

}  // namespace

void corpus::build(uint32_t seed, size_t entries, size_t max_bytes)
{
    using namespace corpus_data;

    lcg r(seed);
    lines_.clear();
    lines_.reserve(entries);
    total_ = 0;
    min_ = static_cast<size_t>(-1);
    max_ = 0;

    for (size_t i = 0; i < entries; ++i) {
        const uint32_t k = r.roll();
        std::string s;

        if (k < 45) {
            // Reactions, sometimes doubled the way people actually type them.
            s = pick_from(r, k_tiny, k_tiny_n);
            if (r.roll() < 25) { s += " "; s += pick_from(r, k_tiny, k_tiny_n); }
        } else if (k < 75) {
            s = pick_from(r, k_short, k_short_n);
        } else if (k < 94) {
            s = pick_from(r, k_mid, k_mid_n);
        } else if (k < 99) {
            // Two sentences run together — the "I have one more thing" shape.
            s = pick_from(r, k_mid, k_mid_n);
            s += " ";
            s += pick_from(r, k_mid, k_mid_n);
        } else if (r.roll() < 70) {
            s = pick_from(r, k_long, k_long_n);
        } else {
            // Just under the payload ceiling. Rare on purpose: it is the
            // boundary case, not the common case, and a corpus that served it
            // often would be measuring the cap rather than chat.
            s = pick_from(r, k_xlong, k_xlong_n);
        }

        // Truncation is on a byte budget, so cut back to a UTF-8 boundary
        // rather than splitting a character. A split character would be a
        // malformed payload, which is a different experiment than this one.
        if (s.size() > max_bytes) {
            size_t cut = max_bytes;
            while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
                --cut;
            s.resize(cut);
        }

        total_ += s.size();
        min_ = std::min(min_, s.size());
        max_ = std::max(max_, s.size());
        lines_.push_back(std::move(s));
    }
    if (lines_.empty()) min_ = 0;
}
