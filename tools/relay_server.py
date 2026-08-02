#!/usr/bin/env python3
"""
Minimal UDP relay for DXX-Redux-SNG, for players who can't port-forward and
don't want to use a VPN.

Both the host and any joining clients only ever talk *outbound* to this
server, which is what makes it work regardless of either side's NAT/router
setup: the game's own home-grown NAT traversal (ICE via GameNetworkingSockets,
plus the "Retro Protocol" P2P mesh) needs a signaling/rendezvous point that
both sides can already reach before it can even attempt a direct route, and
a home router with no forwarded ports and no UPnP can't be that point. This
server is that point -- run it anywhere with a public IP (a cheap VPS is
plenty), open one UDP port on it, and both the host and clients configure
the game to point at it.

Wire protocol (all messages are single UDP datagrams, big-endian ints):

  Client -> Relay   HELLO_HOST     [0x01][token:4]
  Relay  -> Client  HELLO_HOST_ACK [0x81][token:4]

  Client -> Relay   HELLO_CLIENT     [0x02][token:4]
  Relay  -> Client  HELLO_CLIENT_ACK [0x82][token:4][participant:1]
  Relay  -> Client  HELLO_CLIENT_NACK[0x83][token:4]   (no such host / session full)

  Client -> Relay   DATA_TO_HOST   [0x10][token:4][payload...]   (from a joined client)
  Relay  -> Host    DATA_FROM_PART [0x11][token:4][participant:1][payload...]
  Host   -> Relay   DATA_TO_PART   [0x11][token:4][participant:1][payload...]
  Relay  -> Client  DATA_FROM_HOST [0x10][token:4][payload...]

The relay never looks inside `payload` -- it's opaque to it. Everything
above `dxx_sendto`/`dxx_recvfrom` in the game is completely unaware relay
mode is even active.

Usage: relay_server.py [--port 42500] [--bind 0.0.0.0]
No third-party dependencies -- stdlib only.
"""

import argparse
import socket
import struct
import time

HELLO_HOST = 0x01
HELLO_HOST_ACK = 0x81
HELLO_CLIENT = 0x02
HELLO_CLIENT_ACK = 0x82
HELLO_CLIENT_NACK = 0x83
DATA_TO_HOST = 0x10
DATA_FROM_HOST = 0x10
DATA_FROM_PART = 0x11
DATA_TO_PART = 0x11

MAX_PARTICIPANTS = 8
SESSION_TTL = 60.0  # seconds of inactivity before a host/client registration expires


class Session:
    def __init__(self, token):
        self.token = token
        self.host_addr = None
        self.host_last_seen = 0.0
        # participant id (1..8) -> (addr, last_seen)
        self.clients = {}

    def touch_host(self, addr):
        self.host_addr = addr
        self.host_last_seen = time.time()

    def touch_client(self, participant, addr):
        self.clients[participant] = (addr, time.time())

    def next_participant_id(self):
        used = set(self.clients.keys())
        for i in range(1, MAX_PARTICIPANTS + 1):
            if i not in used:
                return i
        return None

    def expire(self):
        now = time.time()
        if self.host_addr is not None and now - self.host_last_seen > SESSION_TTL:
            self.host_addr = None
        for pid in list(self.clients.keys()):
            addr, last_seen = self.clients[pid]
            if now - last_seen > SESSION_TTL:
                del self.clients[pid]

    def is_empty(self):
        return self.host_addr is None and not self.clients


def main():
    ap = argparse.ArgumentParser(description="DXX-Redux-SNG relay server")
    ap.add_argument("--port", type=int, default=42500)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.setblocking(True)
    sock.settimeout(5.0)

    sessions = {}  # token -> Session
    print(f"relay_server listening on {args.bind}:{args.port}")

    last_cleanup = time.time()

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            data = None

        now = time.time()
        if now - last_cleanup > 10:
            last_cleanup = now
            for token in list(sessions.keys()):
                sessions[token].expire()
                if sessions[token].is_empty():
                    del sessions[token]

        if not data or len(data) < 5:
            continue

        msg_type = data[0]
        (token,) = struct.unpack(">I", data[1:5])
        body = data[5:]

        sess = sessions.get(token)

        if msg_type == HELLO_HOST:
            if sess is None:
                sess = sessions[token] = Session(token)
            sess.touch_host(addr)
            sock.sendto(bytes([HELLO_HOST_ACK]) + struct.pack(">I", token), addr)

        elif msg_type == HELLO_CLIENT:
            if sess is None or sess.host_addr is None:
                sock.sendto(bytes([HELLO_CLIENT_NACK]) + struct.pack(">I", token), addr)
                continue
            # Reuse an existing participant id if this address already joined
            existing = None
            for pid, (a, _) in sess.clients.items():
                if a == addr:
                    existing = pid
                    break
            pid = existing if existing is not None else sess.next_participant_id()
            if pid is None:
                sock.sendto(bytes([HELLO_CLIENT_NACK]) + struct.pack(">I", token), addr)
                continue
            sess.touch_client(pid, addr)
            sock.sendto(bytes([HELLO_CLIENT_ACK]) + struct.pack(">I", token) + bytes([pid]), addr)

        elif msg_type == DATA_TO_HOST:
            if sess is None or sess.host_addr is None:
                continue
            pid = None
            for p, (a, _) in sess.clients.items():
                if a == addr:
                    pid = p
                    break
            if pid is None:
                continue  # unknown client, not registered -- drop
            sess.touch_client(pid, addr)
            sock.sendto(bytes([DATA_FROM_PART]) + struct.pack(">I", token) + bytes([pid]) + body, sess.host_addr)

        elif msg_type == DATA_TO_PART:
            if sess is None or addr != sess.host_addr:
                continue  # only the registered host may address participants
            if len(body) < 1:
                continue
            pid = body[0]
            payload = body[1:]
            client = sess.clients.get(pid)
            if client is None:
                continue
            sess.touch_host(addr)
            sock.sendto(bytes([DATA_FROM_HOST]) + struct.pack(">I", token) + payload, client[0])

        # unknown message types are silently dropped


if __name__ == "__main__":
    main()
