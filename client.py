#!/usr/bin/env python3
"""Test client for the epoll chat server.

Modes:
  interactive   one client, type lines, see the room
  load          N clients chatting in a room; checks nothing is lost
  slowreader    connects, joins, then STOPS READING — forces the server's
                send buffer to grow, which is what exercises EPOLLOUT arming
                and the backpressure drop path
  dribble       sends one byte at a time, so every frame arrives split across
                many recv() calls — exercises the partial-frame parser
"""

import argparse
import socket
import struct
import sys
import threading
import time

C_SET_NICK, C_JOIN, C_CHAT = 1, 2, 3
S_NOTICE, S_CHAT = 100, 101

HDR = struct.Struct("<HH")  # len, type


def frame(mtype: int, payload: str) -> bytes:
    body = payload.encode()
    return HDR.pack(len(body), mtype) + body


def read_frames(sock, buf: bytearray):
    """Pull whatever is available and yield complete frames."""
    data = sock.recv(65536)
    if not data:
        return None
    buf.extend(data)
    out = []
    while len(buf) >= HDR.size:
        length, mtype = HDR.unpack_from(buf, 0)
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
        s.sendall(frame(C_SET_NICK, nick))
    if room:
        s.sendall(frame(C_JOIN, room))
    return s


# ------------------------------------------------------------------ modes

def mode_interactive(a):
    s = connect(a.host, a.port, a.nick, a.room)
    buf = bytearray()

    def rx():
        while True:
            frames = read_frames(s, buf)
            if frames is None:
                print("\n[server closed]")
                return
            for mtype, payload in frames:
                tag = "*" if mtype == S_NOTICE else " "
                print(f"{tag} {payload}")

    threading.Thread(target=rx, daemon=True).start()
    print(f"connected as {a.nick} in #{a.room}. type to chat, Ctrl-D to quit.")
    try:
        for line in sys.stdin:
            line = line.rstrip("\n")
            if line:
                s.sendall(frame(C_CHAT, line))
    except KeyboardInterrupt:
        pass
    s.close()


def mode_load(a):
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
            except OSError:
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
                s.sendall(frame(C_CHAT, f"msg {round_no} from bot{i:03d}"))
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
    """The important one. A client that never reads makes the server's `out`
    buffer grow: first the kernel socket buffer fills, then send() returns
    EAGAIN, then the server must arm EPOLLOUT and hold the tail. Once the held
    tail exceeds the cap, the server should drop this client and keep serving
    everyone else."""
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
                    t.sendall(frame(C_CHAT, payload))
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
        probe.sendall(frame(C_CHAT, "still alive?"))
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
    for chunk in (frame(C_SET_NICK, "dribbler"), frame(C_JOIN, a.room)):
        for b in chunk:
            s.sendall(bytes([b]))
            time.sleep(0.002)

    msg = frame(C_CHAT, "one byte at a time")
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
    p = argparse.ArgumentParser()
    p.add_argument("mode", choices=["interactive", "load", "slowreader", "dribble"])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--nick", default="me")
    p.add_argument("--room", default="lobby")
    p.add_argument("--clients", type=int, default=20)
    p.add_argument("--messages", type=int, default=50)
    p.add_argument("--delay", type=float, default=0.01)
    a = p.parse_args()

    {"interactive": mode_interactive,
     "load": mode_load,
     "slowreader": mode_slowreader,
     "dribble": mode_dribble}[a.mode](a)


if __name__ == "__main__":
    main()
