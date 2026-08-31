#!/usr/bin/env python3
"""Human- and correctness-facing client for the chat servers.

`loadgen` is the measuring instrument: it embeds a 16-byte blob, reads the
timestamp back out, and throws the rest away. It will happily report 100%
delivery and a 0.1 ms p99 while every message is arriving at the wrong client
with the wrong body, because it never looks at the content. This file is the
other half — the judge rather than the instrument. It has no performance
requirement at all, which is why it is Python.

Modes:
  interactive   one client, type lines, see the room. The human-eyeball check.
  verify        N clients in a room; asserts every message arrives at every
                member exactly once, with the exact body and the right sender
  load          N clients chatting; counts frames only (delivery, not content)
  slowreader    connects, joins, then STOPS READING — forces the server's send
                buffer to grow, exercising backpressure and the drop path
  dribble       sends one byte at a time, so every frame arrives split across
                many recv() calls — exercises the partial-frame parser

`dribble` matters more for the io_uring port than it looks. The partial-frame
state machine is one of the things that ports verbatim, and there it has to
survive completion semantics on top: a buffer handed to the kernel is
untouchable until the CQE arrives.
"""

import argparse
import socket
import struct
import sys
import threading
import time
from collections import Counter

HDR = struct.Struct("<HH")


class Proto:
    """The one real difference between the two servers.

    Both use a 4-byte little-endian header, no byte swapping. They differ only
    in whether the length field counts the header:

        study     [uint16 len ][uint16 type]   len  = payload bytes
        iouring   [uint16 size][uint16 id  ]   size = payload + header

    Getting this backwards desynchronises the stream by four bytes per frame,
    so every parse after the first is garbage.
    """

    def __init__(self, name):
        if name not in ("study", "iouring"):
            raise ValueError(f"unknown proto {name}")
        self.name = name
        self.len_includes_header = (name == "iouring")

        # These are epoll-chat-study's IDs. iouring-net-lib assigns its own
        # from a schema that does not exist yet (v1 has join / chat / leave
        # with gameplay packets gated behind S_ENTER_WORLD_OK). Fill them in
        # from the generated table rather than guessing — an unrecognised ID
        # closes the session there, so a wrong guess presents as a connection
        # failure and gets debugged as a network problem.
        self.C_SET_NICK, self.C_JOIN, self.C_CHAT = 1, 2, 3
        self.S_NOTICE, self.S_CHAT = 100, 101

    def frame(self, mtype: int, payload) -> bytes:
        body = payload.encode() if isinstance(payload, str) else payload
        n = len(body) + HDR.size if self.len_includes_header else len(body)
        return HDR.pack(n, mtype) + body

    def payload_len(self, raw: int):
        """Payload byte count for a length field that read as `raw`, or None
        if the frame is malformed under this convention."""
        if not self.len_includes_header:
            return raw
        return raw - HDR.size if raw >= HDR.size else None


P = Proto("study")   # replaced in main() by --proto

# The server's payload cap applies to what it BROADCASTS, not to what you
# send: it assembles "nick: " + text and checks that. So the text a client may
# actually send is the cap minus its own nickname — the limit is per-user, and
# nobody is told what theirs is.
#
# Measured against epoll-chat-study with a 6-character nick:
#
#   text <= 1016 B   ->  broadcast 1024 B    ->  delivered
#   text 1017-1024 B ->  broadcast over cap  ->  SILENTLY DROPPED, socket open
#   text >= 1025 B   ->  frame over cap      ->  CONNECTION CLOSED
#
# Two cliffs, both invisible from the client. Checking here turns the first
# into a message and stops the second from happening at all.
MAX_PAYLOAD = 1024
NICK_SEP = ": "


def chat_budget(nick: str) -> int:
    """Bytes of text this nickname may send before the server drops it."""
    return MAX_PAYLOAD - len((nick or "").encode()) - len(NICK_SEP.encode())


def encode_line(line: str):
    """Bytes for a line read from a terminal, plus a warning if it was damaged.

    stdin decodes with errors='surrogateescape' whenever the locale is not a
    full UTF-8 one — C.UTF-8 counts, and that is the WSL default. A byte that
    could not be decoded survives as a lone surrogate, and .encode() on that
    raises UnicodeEncodeError rather than producing bytes. Typing Korean
    through an IME reaches this: a composing syllable can be flushed
    mid-character, so a three-byte sequence arrives with two of its bytes.

    This is the same shape as lesson 4 on the server side. stdin is a byte
    stream, not a character stream, and a multi-byte character can be split
    across reads. The server was given a state machine for exactly that; the
    client was not, and it crashed on the first Korean a human typed into it.
    """
    try:
        return line.encode(), None
    except UnicodeEncodeError:
        # Recover the bytes the terminal actually sent. If they are valid
        # UTF-8 after all, the surrogates were an artifact of how stdin was
        # decoded and the input itself was fine.
        raw = line.encode("utf-8", "surrogateescape")
        try:
            raw.decode("utf-8")
            return raw, None
        except UnicodeDecodeError:
            # Genuinely malformed. Drop the damaged characters rather than the
            # line, and never the session: a chat client that dies on one bad
            # keystroke is worse than one that says what it dropped.
            return (raw.decode("utf-8", "replace").encode(),
                    "input had a truncated UTF-8 sequence "
                    "(IME mid-composition?); damaged characters were replaced")


def read_frames(sock, buf: bytearray):
    """Pull whatever is available and return complete frames, or None on EOF."""
    data = sock.recv(65536)
    if not data:
        return None
    buf.extend(data)
    out = []
    while len(buf) >= HDR.size:
        raw, mtype = HDR.unpack_from(buf, 0)
        length = P.payload_len(raw)
        if length is None:
            raise ValueError(
                f"malformed frame (size={raw} < header) — --proto is probably "
                f"set the wrong way round; the stream is now unrecoverable")
        if len(buf) < HDR.size + length:
            break
        payload = bytes(buf[HDR.size:HDR.size + length]).decode(errors="replace")
        del buf[:HDR.size + length]
        out.append((mtype, payload))
    return out


def connect(host, port, nick=None, room=None, rcvbuf=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # SO_RCVBUF must be set BEFORE connect(): it determines the receive window
    # advertised during the handshake. Setting it afterwards is largely
    # cosmetic, which is why the first backpressure run never triggered.
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if nick:
        s.sendall(P.frame(P.C_SET_NICK, nick))
    if room:
        s.sendall(P.frame(P.C_JOIN, room))
    return s


# ------------------------------------------------------------------ modes

def mode_interactive(a):
    s = connect(a.host, a.port, a.nick, a.room)
    buf = bytearray()
    quitting = threading.Event()

    def rx():
        while not quitting.is_set():
            try:
                frames = read_frames(s, buf)
            except ValueError as e:
                print(f"\n[protocol error] {e}")
                return
            except OSError:
                # Ctrl-D closes the socket from the main thread while this one
                # is parked in recv(), so the fd goes bad underneath it. That
                # is a normal exit, not a fault — but only ValueError was
                # caught here, so every clean quit ended by dumping a
                # traceback that the interpreter then truncated on its way
                # out, which reads exactly like a crash.
                if not quitting.is_set():
                    print("\n[connection lost]")
                return
            if frames is None:
                print("\n[server closed]")
                return
            for mtype, payload in frames:
                tag = "*" if mtype == P.S_NOTICE else " "
                print(f"{tag} {payload}")

    threading.Thread(target=rx, daemon=True).start()
    budget = chat_budget(a.nick)
    print(f"connected as {a.nick} in #{a.room}. type to chat, Ctrl-D to quit.")
    print(f"  message limit {budget} bytes "
          f"(~{budget // 3} Korean characters, {budget} ASCII) — "
          f"your nickname is spent from the same budget.")
    try:
        for line in sys.stdin:
            line = line.rstrip("\n")
            if not line:
                continue

            body, warning = encode_line(line)
            if warning:
                print(f"[input] {warning}")

            # Refuse rather than let the server drop it silently or hang up.
            if len(body) > budget:
                print(f"[too long] {len(body)} bytes, limit {budget} "
                      f"({len(line)} characters). Not sent — the server would "
                      f"have {'closed the connection' if len(body) >= MAX_PAYLOAD + 1 else 'dropped it without telling you'}.")
                continue

            try:
                s.sendall(P.frame(P.C_CHAT, body))
            except OSError as e:
                print(f"\n[send failed] {e}")
                break
    except KeyboardInterrupt:
        pass

    # Stdin ended. A human pressing Ctrl-D has already seen every echo, but a
    # piped script has not: the last message may still be in flight, and
    # closing here would race the reader thread and drop it. A human never
    # notices this pause; a script needs it to be deterministic.
    if not sys.stdin.isatty():
        time.sleep(0.3)
    quitting.set()
    s.close()


def mode_verify(a):
    """Content correctness, which loadgen cannot check.

    Every client sends identifiable messages of varying length; every client
    must receive every message from every member of the room exactly once,
    with the exact body and the correct sender attribution. This is what
    catches cross-room leakage, truncation at a length boundary, duplicate
    delivery, and misattribution — all of which a throughput test reports as
    a clean 100%.
    """
    nicks = [f"v{i:03d}" for i in range(a.clients)]
    socks = [connect(a.host, a.port, n, a.room) for n in nicks]
    for s in socks:
        s.settimeout(0.3)
    print(f"verify: {len(socks)} clients in #{a.room}, {a.messages} msgs each, "
          f"proto={P.name}")

    # Vary length across the size classes so framing boundaries get exercised
    # rather than one comfortable payload size.
    classes = [8, 63, 64, 65, 200, 900]

    expected = []           # (sender_nick, body) in send order
    for r in range(a.messages):
        for i, n in enumerate(nicks):
            fill = classes[(r * len(nicks) + i) % len(classes)]
            body = f"{n}#{r}:" + ("abcdefghij" * ((fill // 10) + 1))[:fill]
            expected.append((n, body))

    received = [Counter() for _ in socks]
    stop = threading.Event()

    def drain(idx, s):
        buf = bytearray()
        while not stop.is_set():
            try:
                frames = read_frames(s, buf)
            except socket.timeout:
                continue
            except ValueError as e:
                print(f"[{nicks[idx]}] {e}")
                return
            except OSError:
                return
            if frames is None:
                return
            for mtype, payload in frames:
                if mtype == P.S_CHAT:
                    received[idx][payload] += 1

    threads = [threading.Thread(target=drain, args=(i, s), daemon=True)
               for i, s in enumerate(socks)]
    for t in threads:
        t.start()

    sent = 0
    for r in range(a.messages):
        for i, s in enumerate(socks):
            n = nicks[i]
            body = expected[r * len(nicks) + i][1]
            try:
                s.sendall(P.frame(P.C_CHAT, body))
                sent += 1
            except OSError:
                pass
        time.sleep(a.delay)

    time.sleep(1.5)
    stop.set()

    # The server broadcasts "nick: body" to the whole room, sender included.
    want = Counter(f"{n}: {b}" for n, b in expected)

    missing = dupes = corrupt = 0
    for idx, got in enumerate(received):
        for line, need in want.items():
            have = got.get(line, 0)
            if have < need:
                missing += need - have
            elif have > need:
                dupes += have - need
        for line in got:
            if line not in want:
                corrupt += got[line]

    total_want = sum(want.values()) * len(socks)
    total_got = sum(sum(c.values()) for c in received)
    print(f"sent {sent} chats; expected {total_want} deliveries, saw {total_got}")
    print(f"  missing (never arrived)      : {missing}")
    print(f"  duplicated                   : {dupes}")
    print(f"  unrecognised body or sender  : {corrupt}")

    ok = (missing == 0 and dupes == 0 and corrupt == 0)
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    for s in socks:
        s.close()
    return 0 if ok else 1


def mode_load(a):
    """Frame counting only. Says nothing about content — that is `verify`."""
    socks = []
    for i in range(a.clients):
        socks.append(connect(a.host, a.port, f"bot{i:03d}", a.room))
    print(f"connected {len(socks)} clients to #{a.room}")

    received = [0] * len(socks)
    stop = threading.Event()

    def drain(idx, s):
        buf = bytearray()
        s.settimeout(0.5)
        while not stop.is_set():
            try:
                frames = read_frames(s, buf)
                if frames is None:
                    return
                received[idx] += len(frames)
            except socket.timeout:
                continue
            except (OSError, ValueError):
                return

    threads = [threading.Thread(target=drain, args=(i, s), daemon=True)
               for i, s in enumerate(socks)]
    for t in threads:
        t.start()

    start = time.time()
    sent = 0
    for round_no in range(a.messages):
        for i, s in enumerate(socks):
            try:
                s.sendall(P.frame(P.C_CHAT, f"msg {round_no} from bot{i:03d}"))
                sent += 1
            except OSError:
                pass
        time.sleep(a.delay)

    time.sleep(1.5)
    stop.set()
    elapsed = time.time() - start

    total_rx = sum(received)
    expected = sent * len(socks)   # every chat is echoed to the whole room
    print(f"sent {sent} chats in {elapsed:.2f}s")
    print(f"received {total_rx} frames (expected ~{expected} + notices)")
    print(f"delivery: {100.0 * total_rx / max(expected, 1):.1f}%")
    for s in socks:
        s.close()


def mode_slowreader(a):
    """A client that never reads makes the server's `out` buffer grow: first
    the kernel socket buffer fills, then send() returns EAGAIN, then the server
    must hold the tail. Once the held tail exceeds the cap, the server should
    drop this client and keep serving everyone else.

    On loopback the kernel auto-tunes SO_SNDBUF to megabytes, so the server
    swallows everything and this never triggers — the study server has
    CHAT_SNDBUF for exactly that reason."""
    victim = connect(a.host, a.port, "victim", a.room, rcvbuf=2048)

    talkers = [connect(a.host, a.port, f"talk{i}", a.room) for i in range(a.clients)]
    for t in talkers:
        t.settimeout(0.2)
    print(f"victim connected and will never read; {len(talkers)} talkers flooding #{a.room}")

    payload = "x" * 900
    sent = 0
    try:
        for _ in range(a.messages):
            for t in talkers:
                try:
                    t.sendall(P.frame(P.C_CHAT, payload))
                    sent += 1
                except OSError:
                    pass
            # talkers drain themselves so only the victim backs up
            for t in talkers:
                try:
                    t.recv(65536)
                except (socket.timeout, OSError):
                    pass
    except KeyboardInterrupt:
        pass

    print(f"flooded {sent} messages; watch the server log for a [drop] on the victim")
    time.sleep(1.0)

    # Is the server still healthy for a brand-new client?
    try:
        probe = connect(a.host, a.port, "probe", a.room)
        probe.settimeout(2.0)
        probe.sendall(P.frame(P.C_CHAT, "still alive?"))
        buf = bytearray()
        got = read_frames(probe, buf)
        print(f"post-flood probe: server responsive, {len(got or [])} frames back")
        probe.close()
    except OSError as e:
        print(f"post-flood probe FAILED: {e}")

    victim.close()
    for t in talkers:
        t.close()


def mode_dribble(a):
    """Send frames one byte at a time. If the server's parser assumes a frame
    arrives whole, this breaks it immediately."""
    s = connect(a.host, a.port)
    for chunk in (P.frame(P.C_SET_NICK, "dribbler"), P.frame(P.C_JOIN, a.room)):
        for b in chunk:
            s.sendall(bytes([b]))
            time.sleep(0.002)

    msg = P.frame(P.C_CHAT, "one byte at a time")
    for b in msg:
        s.sendall(bytes([b]))
        time.sleep(0.002)

    s.settimeout(2.0)
    buf = bytearray()
    try:
        frames = read_frames(s, buf) or []
        for mtype, payload in frames:
            print(f"< [{mtype}] {payload}")
        print("dribble OK — server reassembled split frames")
    except socket.timeout:
        print("dribble FAILED — no response, parser likely stalled")
    s.close()


def main():
    global P
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["interactive", "verify", "load",
                                    "slowreader", "dribble"])
    p.add_argument("--proto", default="study", choices=["study", "iouring"],
                   help="framing convention; see Proto in this file")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--nick", default="me")
    p.add_argument("--room", default="lobby")
    p.add_argument("--clients", type=int, default=20)
    p.add_argument("--messages", type=int, default=50)
    p.add_argument("--delay", type=float, default=0.01)
    a = p.parse_args()

    P = Proto(a.proto)

    rc = {"interactive": mode_interactive,
          "verify": mode_verify,
          "load": mode_load,
          "slowreader": mode_slowreader,
          "dribble": mode_dribble}[a.mode](a)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
