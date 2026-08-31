#!/usr/bin/env python3
"""Compile the authored chat text in .corpus-src/ into src/corpus_data.cpp.

The pool is generated rather than hand-edited because the one property that
matters -- which byte-length bucket a line lands in -- is not visible when you
are writing the line. A human writing Korean counts characters; the protocol
counts bytes, and the two differ by 3x. So the authoring format is a directory
of plain .txt files with one message per line, and this script does the
bucketing, the deduplication, and the escaping.

    python3 tools/mkcorpus.py

Both the source directory and the generated corpus_data.cpp are committed. The
text is stored twice and that is the intended trade: the alternative is either a
generator with no inputs, which can never run again, or a Python step wired into
a C++ build. Add lines to .corpus-src/ and re-run this script -- do not edit
corpus_data.cpp, because the bucket a line belongs in is decided by its byte
length and that is not something you can judge by eye in Korean.

Buckets are byte ranges, not filenames, so a file whose lines came out longer
than intended still lands where the wire says it belongs.
"""

import collections
import os
import random
import sys

# Must match k_max_payload - k_prefix_slack - k_blob_header in src/wire.h. A
# line over this is silently truncated at runtime, which would cut a UTF-8
# character in half; refuse it here instead, where it is fixable.
MAX_BYTES = 1024 - 16 - 20

# (name, lo, hi) inclusive byte bounds. The gaps are intentional: the classes
# are meant to be distinct points on the size curve, and a line that lands
# between two of them is a line that was written to the wrong spec.
BUCKETS = [
    ("tiny",    1,  15),
    ("short",  16,  64),
    ("mid",    65, 200),
    ("long",  201, 700),
    ("xlong", 701, MAX_BYTES),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, ".corpus-src")
OUT = os.path.join(ROOT, "src", "corpus_data.cpp")


def escape(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "?":
            out.append("\\?")      # keeps "??(" out of trigraph territory
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def ngram_ratio(lines, n=4):
    """Unique n-grams over total n-grams."""
    g = collections.Counter()
    for l in lines:
        for i in range(len(l) - n + 1):
            g[l[i:i + n]] += 1
    total = sum(g.values())
    return (len(g) / total if total else 0.0), total


# The raw ratio cannot be compared across buckets, because it falls as the
# sample grows for any natural language: the stock of common 4-grams is finite
# while the text is not. Two corrections are needed, and the second is the one
# that is easy to get wrong.
#
# First, sample size. 175 lines of this Korean chat score ~0.85 whether they
# came from one batch or four, but the 876-line union of four such batches
# scores 0.69 -- same text, same quality, lower number.
#
# Second, the unit of sampling has to be 4-grams, not lines. A fixed line count
# is not a fixed sample: an xlong line carries ~320 4-grams and a short line
# ~11, so "150 lines" means 19500 grams for one bucket and 1650 for another,
# and the longer bucket scores lower for that reason alone. Normalising on
# lines made k_long look like the worst bucket in the pool when it was merely
# the one with the most text per line.
TARGET_GRAMS = 4000
SAMPLE_ROUNDS = 20


def diversity(lines):
    """Mean 4-gram ratio over subsamples holding the 4-gram count fixed.

    This is the check that matters for a generated corpus. "300 unique lines"
    is satisfied by 300 permutations of one sentence with the nouns swapped,
    and such a pool compresses to nothing and exercises one code path. Distinct
    lines are cheap; distinct language is the thing being asked for -- and only
    a sample-matched measurement can tell the two apart.
    """
    grams_per_line = [max(len(l) - 3, 0) for l in lines]
    if sum(grams_per_line) <= TARGET_GRAMS:
        return ngram_ratio(lines)[0]

    rng = random.Random(0)   # fixed, so the printed report is reproducible
    idx = list(range(len(lines)))
    vals = []
    for _ in range(SAMPLE_ROUNDS):
        rng.shuffle(idx)
        take, grams = [], 0
        for i in idx:
            take.append(lines[i])
            grams += grams_per_line[i]
            if grams >= TARGET_GRAMS:
                break
        vals.append(ngram_ratio(take)[0])
    return sum(vals) / len(vals)


def main():
    if not os.path.isdir(SRC_DIR):
        sys.exit(f"{SRC_DIR} not found — it is untracked authoring scratch, "
                 f"see the header of this script")

    seen, buckets, rejected = set(), {b[0]: [] for b in BUCKETS}, []
    files = sorted(f for f in os.listdir(SRC_DIR) if f.endswith(".txt"))
    if not files:
        sys.exit(f"no .txt files in {SRC_DIR}")

    dupes = 0
    for fname in files:
        with open(os.path.join(SRC_DIR, fname), encoding="utf-8") as f:
            for lineno, raw in enumerate(f, 1):
                line = raw.strip()
                if not line:
                    continue
                if line in seen:
                    dupes += 1
                    continue
                seen.add(line)
                nb = len(line.encode("utf-8"))
                for name, lo, hi in BUCKETS:
                    if lo <= nb <= hi:
                        buckets[name].append((nb, line))
                        break
                else:
                    rejected.append((fname, lineno, nb, line[:40]))

    if rejected:
        print(f"[mkcorpus] {len(rejected)} line(s) fit no bucket:")
        for fname, lineno, nb, head in rejected[:20]:
            print(f"  {fname}:{lineno} {nb}B  {head}…")
        sys.exit("[mkcorpus] refusing to generate — fix the lengths first")

    empty = [n for n in buckets if not buckets[n]]
    if empty:
        # An empty bucket is not a degenerate corpus, it is a missing one: the
        # sampler in corpus.cpp indexes every bucket unconditionally, so a
        # zero-length array is an out-of-bounds read on the first draw.
        sys.exit(f"[mkcorpus] no lines for bucket(s): {', '.join(empty)} — "
                 f"the sampler reads every bucket, so this would be UB")

    for name in buckets:
        # Sorted so the generated file is a function of the input set alone.
        # Array order feeds the seeded sampler, so an unstable order would make
        # two regenerations produce different traffic from the same text.
        buckets[name].sort()

    with open(OUT, "w", encoding="utf-8") as out:
        out.write(
            "// corpus_data.cpp — GENERATED by tools/mkcorpus.py. Do not edit.\n"
            "//\n"
            "// Source text is the .corpus-src/ directory: one message\n"
            "// per line, plain UTF-8, bucketed here by byte length. Regenerate\n"
            "// with `python3 tools/mkcorpus.py` after editing that directory.\n"
            "//\n"
            "// The text is Korean chat, hand-authored rather than sampled from a\n"
            "// real log, because a real log is someone's private conversation.\n"
            "// What it has to reproduce is not any particular message but the\n"
            "// length distribution and the multi-byte encoding, since those are\n"
            "// what decide how much work a delivery is.\n"
            "#include \"corpus_data.h\"\n"
            "\n"
            "namespace corpus_data {\n")
        for name, lo, hi in BUCKETS:
            rows = buckets[name]
            nb = [r[0] for r in rows]
            div = diversity([r[1] for r in rows])
            out.write(f"\n// {len(rows)} lines, {min(nb)}..{max(nb)} bytes, "
                      f"mean {sum(nb)/len(nb):.0f}; 4-gram diversity "
                      f"{div:.2f} at {TARGET_GRAMS} 4-grams\n")
            out.write(f"const char* const k_{name}[] = {{\n")
            for _, line in rows:
                out.write(f'    "{escape(line)}",\n')
            out.write("};\n")
            out.write(f"const size_t k_{name}_n = sizeof(k_{name}) "
                      f"/ sizeof(k_{name}[0]);\n")
        out.write("\n}  // namespace corpus_data\n")

    print(f"[mkcorpus] {len(files)} files, {len(seen)} unique lines"
          + (f", {dupes} duplicate(s) dropped" if dupes else ""))
    worst = 1.0
    for name, lo, hi in BUCKETS:
        rows = buckets[name]
        nb = [r[0] for r in rows]
        div = diversity([r[1] for r in rows])
        raw, _ = ngram_ratio([r[1] for r in rows])
        # k_tiny is exempt: reactions are 2-4 bytes, mostly shorter than the
        # 4-gram window, so the measure has almost nothing to chew on and says
        # 1.00 regardless. Its diversity is the line count, and that is visible.
        if name != "tiny":
            worst = min(worst, div)
        flag = "  <-- LOW DIVERSITY" if div < 0.75 and name != "tiny" else ""
        print(f"  k_{name:<6} {len(rows):>5} lines  {min(nb):>4}..{max(nb):<4}B"
              f"  mean {sum(nb)/len(nb):>5.0f}B  4gram {div:.2f}"
              f" (raw {raw:.2f}){flag}")
    print(f"[mkcorpus] worst sample-matched bucket diversity {worst:.2f} "
          f"(natural Korean chat measures ~0.85; well below that means the "
          f"lines are permutations of a template, not distinct language)")
    print(f"[mkcorpus] wrote {os.path.relpath(OUT, ROOT)} "
          f"({os.path.getsize(OUT)/1024:.1f} KiB)")


if __name__ == "__main__":
    main()
