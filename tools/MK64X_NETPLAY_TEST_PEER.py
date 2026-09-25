#!/usr/bin/env python3
r"""MK64X protocol-v4 handshake test peer.

This does NOT emulate Mario Kart gameplay.  It only proves that the OG Xbox
menu/UDP/session handshake is wire-compatible with the Xbox 360 MK4P protocol.

Examples:
  # Pretend to be a joining console against an OG Xbox host:
  py tools\MK64X_NETPLAY_TEST_PEER.py join --host 192.168.1.50

  # Pretend to be a host for an OG Xbox choosing JOIN:
  py tools\MK64X_NETPLAY_TEST_PEER.py host
"""
import argparse
import os
import socket
import struct
import sys
import time

MAGIC = b"MK4P"
VERSION = 4
BUILD = 0xB3100915
PORT = 6464
HEADER = 28
HELLO, OFFER, READY, START, START_ACK, CLIENT_INPUT, FRAMESET, GOODBYE, BOOT_READY, BOOT_GO, JOIN_REJECT = range(1, 12)


def now32():
    return int(time.monotonic() * 1000) & 0xFFFFFFFF


def packet(ptype, sid, payload=b""):
    if len(sid) != 16:
        raise ValueError("session/nonce must be 16 bytes")
    n = HEADER + len(payload)
    return MAGIC + bytes((VERSION, ptype, (n >> 8) & 255, n & 255)) + struct.pack(">I", BUILD) + sid + payload


def decode(data):
    if len(data) < HEADER or data[:4] != MAGIC or data[4] != VERSION:
        return None
    n = (data[6] << 8) | data[7]
    if n != len(data) or struct.unpack_from(">I", data, 8)[0] != BUILD:
        return None
    return data[5], data[12:28], data[28:]


def endpoint(a):
    return f"{a[0]}:{a[1]}"


def join_mode(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.bind_port))
    s.settimeout(0.10)
    target = (args.host, PORT)
    nonce = os.urandom(16)
    requested = bytes((args.local_players,))
    session = None
    assigned = None
    offer_payload = None
    last_hello = 0.0
    started = time.monotonic()
    print(f"[JOIN] local UDP {endpoint(s.getsockname())} -> OG host {endpoint(target)}")
    print(f"[JOIN] requesting {args.local_players} racer slot(s)")

    while time.monotonic() - started < args.timeout:
        now = time.monotonic()
        if session is None and now - last_hello >= 0.25:
            s.sendto(packet(HELLO, nonce, requested), target)
            last_hello = now
        try:
            data, src = s.recvfrom(2048)
        except socket.timeout:
            continue
        d = decode(data)
        if not d:
            continue
        ptype, sid, payload = d
        if ptype == JOIN_REJECT and sid == nonce:
            print("[JOIN] host rejected reservation (lobby full)")
            return 2
        if ptype == OFFER and len(payload) == 24 and payload[:16] == nonce:
            session = sid
            offer_payload = payload
            assigned = payload[20]
            s.sendto(packet(READY, session, offer_payload), src)
            print(f"[JOIN] OFFER from {endpoint(src)} -> assigned P{assigned+1}; READY sent")
            target = src
            continue
        if ptype == START and session is not None and sid == session and len(payload) == 5:
            delay, players, slot, local_minus1, split = payload
            if assigned is not None and slot != assigned:
                continue
            s.sendto(packet(START_ACK, session, bytes((slot,))), src)
            print(f"[JOIN] START received: players={players} slot=P{slot+1} local={local_minus1+1} delay={delay} split={split}")
            print("[JOIN] START_ACK sent - HANDSHAKE PASS")
            return 0
    print("[JOIN] timeout - no complete handshake")
    return 1


def host_mode(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    s.settimeout(0.10)
    session = os.urandom(16)
    peer = None
    peer_nonce = None
    requested = 1
    offer = None
    ready = False
    last_offer = 0.0
    last_start = 0.0
    ready_at = 0.0
    started = time.monotonic()
    print(f"[HOST] listening on UDP 0.0.0.0:{PORT}")
    print("[HOST] On OG Xbox choose JOIN 2-4 PLAYER GAME and enter this PC's LAN IPv4")

    while time.monotonic() - started < args.timeout:
        now = time.monotonic()
        if ready and peer and now - ready_at >= args.start_delay and now - last_start >= 0.10:
            players = 1 + requested
            split = 1 if requested == 2 else 0
            payload = bytes((args.delay, players, 1, requested - 1, split))
            s.sendto(packet(START, session, payload), peer)
            last_start = now
        elif peer and not ready and offer and now - last_offer >= 1.0:
            s.sendto(packet(OFFER, session, offer), peer)
            last_offer = now

        try:
            data, src = s.recvfrom(2048)
        except socket.timeout:
            continue
        d = decode(data)
        if not d:
            continue
        ptype, sid, payload = d
        if ptype == HELLO and len(payload) == 1 and 1 <= payload[0] <= 2:
            requested = payload[0]
            peer = src
            peer_nonce = sid
            stamp = now32()
            # nonce[16], stamp[4], slot[1], current lobby slots[1], locals-1[1], reserved[1]
            offer = peer_nonce + struct.pack(">I", stamp) + bytes((1, 1 + requested, requested - 1, 0))
            s.sendto(packet(OFFER, session, offer), peer)
            last_offer = now
            print(f"[HOST] HELLO from {endpoint(src)} requesting {requested} slot(s) -> OFFER P2")
            continue
        if ptype == READY and peer and src == peer and sid == session and offer and payload == offer:
            ready = True
            ready_at = now
            print(f"[HOST] READY from {endpoint(src)}; START will be sent")
            continue
        if ptype == START_ACK and ready and peer and src == peer and sid == session and payload == b"\x01":
            print("[HOST] START_ACK received - HANDSHAKE PASS")
            return 0
    print("[HOST] timeout - no complete handshake")
    return 1


def main():
    ap = argparse.ArgumentParser(description="MK64X MK4P v4 handshake test peer")
    sub = ap.add_subparsers(dest="mode", required=True)
    j = sub.add_parser("join", help="simulate a guest joining an OG Xbox host")
    j.add_argument("--host", required=True, help="OG Xbox host IPv4")
    j.add_argument("--local-players", type=int, choices=(1, 2), default=1)
    j.add_argument("--bind-port", type=int, default=0, help="local UDP port; 0 = automatic")
    j.add_argument("--timeout", type=float, default=60.0)
    h = sub.add_parser("host", help="simulate a host for an OG Xbox joiner")
    h.add_argument("--delay", type=int, choices=range(2, 13), default=4)
    h.add_argument("--start-delay", type=float, default=0.5)
    h.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()
    try:
        return join_mode(args) if args.mode == "join" else host_mode(args)
    except KeyboardInterrupt:
        print("\nCancelled")
        return 130
    except OSError as e:
        print(f"Socket error: {e}")
        return 3


if __name__ == "__main__":
    sys.exit(main())
