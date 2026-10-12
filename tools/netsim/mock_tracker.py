#!/usr/bin/env python3
"""Mock DXX tracker + STUN server for tools/netsim.

Speaks just enough of the tracker protocol (see net_udp.c, "Tracker defines")
for register / game list / hole-punch brokering / pre-join ICE signal relay.
STUN answers Binding requests so ICE can learn server-reflexive addresses.
"""
import socket, struct, sys, threading, time

TRACKER_PORT = 42420
STUN_PORT = 3478

REGISTER, UNREGISTER, GAMELIST, HOLEPUNCH, SIGNAL = 0, 1, 2, 6, 7
UPID_LITE_REQ, UPID_LITE = 4, 5
UPID_VERIFY, UPID_INCGAME, UPID_HOLEPUNCH, UPID_SIGNAL = 21, 22, 23, 32
ROLE_HOST_KEEPALIVE, ROLE_CLIENT_REQUEST = 0, 1
IN_HOST_ADDR, IN_CLIENT_ADDR = 0, 1

BIND = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != "--fixed" else "0.0.0.0"  # must be the address clients dial, or NAT drops replies

games = {}  # gameid -> dict(reg_addr, ver, port, punch_addr, lite)


def log(msg):
    print("[tracker %.1f] %s" % (time.time() % 1000, msg), flush=True)


def pack_addr(addr):
    return socket.inet_aton(addr[0]) + struct.pack("!H", addr[1])


def punch_in(role, addr):
    return struct.pack("<BB", UPID_HOLEPUNCH, role) + b"\0" * 4 + pack_addr(addr)


def lite_request(ver):
    return bytes([UPID_LITE_REQ]) + b"D1XR" + ver


def handle_register(sock, data, addr):
    gameid = struct.unpack_from("<I", data, 5)[0]
    port = struct.unpack_from("<H", data, 3)[0]
    g = games.setdefault(gameid, {})
    g.update(reg_addr=addr, port=port, ver=data[9:15], seen=time.time())
    sock.sendto(bytes([UPID_VERIFY]), addr)
    log("register gameid=%d from %s:%d (advertised port %d)" % (gameid, addr[0], addr[1], port))


def handle_gamelist(sock, addr):
    for gameid, g in games.items():
        if "lite" not in g:
            continue
        ip = g["reg_addr"][0]
        pkt = bytes([UPID_INCGAME, 0]) + ip.encode() + b"\0" + struct.pack("<H", g["port"]) + g["lite"][1:]
        sock.sendto(pkt, addr)
    log("gamelist to %s:%d (%d games)" % (addr[0], addr[1], len(games)))


def handle_holepunch(sock, data, addr):
    role = data[2]
    gameid = struct.unpack_from("<I", data, 3)[0]
    g = games.get(gameid)
    if not g:
        return
    if role == ROLE_HOST_KEEPALIVE:
        first = "punch_addr" not in g
        g["punch_addr"] = addr
        if first:
            log("host keepalive gameid=%d mapped to %s:%d" % (gameid, addr[0], addr[1]))
            sock.sendto(lite_request(g["ver"]), addr)
        return
    host = g.get("punch_addr")
    if not host:
        return
    log("client %s:%d wants gameid=%d -> brokering with host %s:%d" % (addr + (gameid,) + host))
    sock.sendto(punch_in(IN_CLIENT_ADDR, addr), host)
    sock.sendto(punch_in(IN_HOST_ADDR, host), addr)


def handle_signal(sock, data, addr):
    gameid = struct.unpack_from("<I", data, 2)[0]
    g = games.get(gameid)
    if not g or "punch_addr" not in g:
        return
    host = g["punch_addr"]
    if addr == host:
        # host -> client: [op][ver][gameid][client_ip:4][client_port:2][blob]
        client = (socket.inet_ntoa(data[6:10]), struct.unpack("!H", data[10:12])[0])
        sock.sendto(bytes([UPID_SIGNAL]) + data[12:], client)
        log("signal host->client %s:%d, %d bytes" % (client + (len(data) - 12,)))
    else:
        sock.sendto(bytes([UPID_SIGNAL]) + pack_addr(addr) + data[6:], host)
        log("signal client %s:%d->host, %d bytes" % (addr + (len(data) - 6,)))


def tracker_loop():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((BIND, TRACKER_PORT))
    log("listening on udp/%d" % TRACKER_PORT)
    while True:
        data, addr = sock.recvfrom(4096)
        if not data:
            continue
        op = data[0]
        if op == REGISTER and len(data) >= 15:
            handle_register(sock, data, addr)
        elif op == UNREGISTER and len(data) >= 5:
            games.pop(struct.unpack_from("<I", data, 1)[0], None)
            log("unregister from %s:%d" % addr)
        elif op == GAMELIST:
            handle_gamelist(sock, addr)
        elif op == HOLEPUNCH and len(data) >= 7:
            handle_holepunch(sock, data, addr)
        elif op == SIGNAL and len(data) >= 6:
            handle_signal(sock, data, addr)
        elif op == UPID_LITE and len(data) != 73 and "--fixed" not in sys.argv:
            log("Unexpected info: len=%d (real tracker only accepts 73)" % len(data))
        elif op == UPID_LITE:
            for g in games.values():
                if g.get("punch_addr") == addr or g["reg_addr"][0] == addr[0]:
                    g["lite"] = data
                    log("cached lite info from %s:%d" % addr)
                    break


def stun_loop():
    magic = 0x2112A442
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((BIND, STUN_PORT))
    log("STUN on udp/%d" % STUN_PORT)
    while True:
        data, addr = sock.recvfrom(2048)
        if len(data) < 20:
            continue
        mtype, _, cookie = struct.unpack_from("!HHI", data, 0)
        if mtype != 0x0001 or cookie != magic:
            continue
        xport = addr[1] ^ (magic >> 16)
        xaddr = struct.unpack("!I", socket.inet_aton(addr[0]))[0] ^ magic
        attr = struct.pack("!HHBBHI", 0x0020, 8, 0, 1, xport, xaddr)
        reply = struct.pack("!HHI", 0x0101, len(attr), magic) + data[8:20] + attr
        sock.sendto(reply, addr)


if __name__ == "__main__":
    threading.Thread(target=stun_loop, daemon=True).start()
    tracker_loop()
