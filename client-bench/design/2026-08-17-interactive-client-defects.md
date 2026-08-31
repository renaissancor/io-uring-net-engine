# 2026-08-17 — Three defects a human found in one minute

Every synthetic mode passed. `verify` reported 1,280/1,280 with exact bodies.
`dribble`, `slowreader`, `load` all green. A three-process fleet moved 60
million frames with zero unparsed. A scripted tmux session with two clients
exchanged Korean and emoji correctly.

Then a person opened the interactive client and typed Korean. It crashed on the
third line.

```
UnicodeEncodeError: 'utf-8' codec can't encode characters in position 1-2:
surrogates not allowed
```

None of the automated modes could have found any of the three defects below,
and the reason is the same for all three: **they only exercise paths a program
drives.** `tmux send-keys` delivers a complete, well-formed UTF-8 line in one
write. A human with an IME does not.

## 1. stdin is a byte stream, and an IME splits characters across reads

**Symptom.** Client dies on a line of Korean. Session gone, other clients see
`* left`.

**Cause.** `sys.stdin` decodes with `errors='surrogateescape'` whenever the
locale is not a full UTF-8 one — `C.UTF-8` counts, and that is the WSL default.
A byte that cannot be decoded survives as a lone surrogate, and `.encode()` on
a lone surrogate raises rather than producing bytes.

Reproduced exactly:

```
input bytes:  \xed\x95\x9c  \xea\xb8
              한 (complete)  first 2 bytes of a 3-byte character
decoded str:  '한\udcea\udcb8'
.encode():    UnicodeEncodeError, position 1-2
```

A Korean IME composes a syllable from jamo and can flush it mid-character. The
user's input showed it plainly: `나 는 밥이야` — the space inside `나는` is
composition state committed at the wrong moment.

**This is lesson 4 wearing different clothes.** "TCP is a byte stream, not a
message stream" — here stdin is the byte stream and a UTF-8 character is the
frame. The server was given a partial-frame state machine for exactly this.
The client was not, because nothing that drives it programmatically ever
produces a partial character.

**Fix.** `encode_line()`: recover the original bytes with `surrogateescape`,
then try a strict decode. If it succeeds the input was fine and the surrogates
were an artifact of how stdin was read. If it fails the input was genuinely
damaged, so replace the bad characters and say so — and never let it kill the
session.

## 2. The payload cap is per-user, and nobody is told theirs

**Cause.** The server caps what it *broadcasts*, not what it receives. It
assembles `"nick: " + text` and then checks 1024. So the text a client may send
is 1024 minus its own nickname minus 2, and there are two separate cliffs.

Measured with a 6-byte nick:

| text | broadcast | what happens |
|---:|---:|---|
| ≤ 1016 B | ≤ 1024 B | delivered |
| 1017–1024 B | 1025–1032 B | **silently dropped.** Socket stays open, no error, message simply never exists |
| ≥ 1025 B | — | **connection closed.** `[warn] oversize frame` server-side |

The width of the middle band is exactly `len("nick: ")`. Rename yourself and
the cliff moves.

**Fix.** `chat_budget(nick)` computes the real limit, the client prints it on
connect, and oversize lines are refused with which cliff they would have hit.
Verified end to end: 1019 B delivered, 1020 B refused as "would be dropped",
1100 B refused as "would close the connection", session survives all three, and
the server logged zero oversize warnings because nothing bad reached it.

**Note for the io_uring port.** This is a protocol design consequence, not a
bug in the study server — the byte cap is the invariant and the character limit
is derived from it, which is the right ordering. But *deriving the user-visible
limit and telling the user* is the client's job, and it had not been done.

## 3. Every clean exit printed a truncated traceback

**Cause.** Ctrl-D closes the socket from the main thread while the reader
thread is parked in `recv()`. The fd goes bad underneath it and it raises
`OSError`. Only `ValueError` was caught, so the thread died — and because the
interpreter was already shutting down, the traceback printed half and stopped.

Which reads exactly like a crash. It happened on **every** normal quit, and it
was invisible in the earlier tmux demo only because killing the pane skipped
the shutdown path entirely.

**Fix.** Catch `OSError`, and use a `quitting` event so a deliberate close is
silent while a real connection loss still reports. Verified over 10 consecutive
clean exits, zero tracebacks.

A fourth thing fell out of testing it: with piped stdin the main thread could
close before the last echoes arrived, so scripted use dropped messages
non-deterministically. Now drains briefly when stdin is not a TTY — 20/20 runs
complete.

## What generalises

**The mode a human drives is the mode that finds the human's bugs.** This is
not a statement about test coverage; `verify` covers more of the protocol than
`interactive` does. It is a statement about *input distribution*. Synthetic
drivers emit well-formed input by construction, because they build it from the
same assumptions the parser holds. Malformed input is exactly what they cannot
generate.

**Two of the three were in shutdown and error paths, not in the feature.** The
chat worked. Sending worked, receiving worked, rooms worked. What broke was
what happens when the input is wrong and what happens when you quit — the parts
with no assertion pointing at them.

**A crash on bad input is a worse bug than the bad input.** Defect 1 was
triggered by a genuinely damaged byte sequence, and the correct response is to
drop those bytes and keep going. Losing the session — and, for other people in
the room, watching a user vanish — is a much larger failure than losing two
characters.

## Rationale links

- `chatcli.py` — `encode_line()`, `chat_budget()`, `mode_interactive`.
- `../server-epoll/README.md` lesson 4 — the same hazard, on the server.
- [`2026-08-17-three-instrument-defects.md`](2026-08-17-three-instrument-defects.md)
  — the other three found the same day, all by disagreement rather than by
  inspection. These three were found by use.
