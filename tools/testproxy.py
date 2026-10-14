#!/usr/bin/env python3
"""testproxy.py - a controllable proxy between an agent and a hub (Phase 3 tests).

    testproxy.py <listen-host:port> <hub-host:port> [--tls CERT KEY] [--prefix /path]
                 [--control FILE]

--tls      terminate TLS with this certificate and key (the agent connects https)
--prefix   act as a reverse proxy publishing the hub under /path: the first
           request line's path must start with it, and it is removed
--control  a file holding one word, re-read every second:
             pass       forward traffic (the default)
             blackhole  stop forwarding in both directions but keep connections
                        open, as a silent network drop does
             reset      close every connection now, then act as "pass"
Only stdlib; logs to stdout.  Stop it with SIGTERM.
"""
import argparse
import os
import select
import socket
import ssl
import sys
import threading
import time

state = {"mode": "pass"}
conns = []
lock = threading.Lock()


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def watch_control(path):
    last = None
    while True:
        try:
            with open(path) as f:
                mode = f.read().strip() or "pass"
        except OSError:
            mode = "pass"
        if mode != last:
            log("mode %s" % mode)
            last = mode
            if mode == "reset":
                with lock:
                    for a, b in conns:
                        for s in (a, b):
                            try:
                                s.close()
                            except OSError:
                                pass
                    conns.clear()
                mode = "pass"
        state["mode"] = mode
        time.sleep(1)


def relay(client, hub_addr, prefix):
    try:
        hub = socket.create_connection(hub_addr, timeout=10)
        hub.settimeout(None)
    except OSError as e:
        log("hub connect failed: %s" % e)
        client.close()
        return
    with lock:
        conns.append((client, hub))
    first = prefix is not None
    pend = {client: b"", hub: b""}
    try:
        while True:
            if state["mode"] == "blackhole":
                time.sleep(0.2)
                continue
            r, _, _ = select.select([client, hub], [], [], 0.5)
            for s in r:
                data = s.recv(65536)
                if not data:
                    return
                if s is client and first:
                    pend[client] += data
                    if b"\r\n" not in pend[client]:
                        continue
                    line, rest = pend[client].split(b"\r\n", 1)
                    method, path, ver = line.split(b" ", 2)
                    if not path.startswith(prefix.encode()):
                        client.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
                        return
                    path = path[len(prefix):] or b"/"
                    data = b" ".join([method, path, ver]) + b"\r\n" + rest
                    first = False
                (hub if s is client else client).sendall(data)
    except (OSError, ValueError):
        pass
    finally:
        for s in (client, hub):
            try:
                s.close()
            except OSError:
                pass
        with lock:
            if (client, hub) in conns:
                conns.remove((client, hub))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("listen")
    ap.add_argument("hub")
    ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
    ap.add_argument("--prefix")
    ap.add_argument("--control")
    a = ap.parse_args()
    lh, lp = a.listen.rsplit(":", 1)
    hh, hp = a.hub.rsplit(":", 1)
    ctx = None
    if a.tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(a.tls[0], a.tls[1])
    if a.control:
        threading.Thread(target=watch_control, args=(a.control,), daemon=True).start()
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((lh, int(lp)))
    ls.listen(8)
    log("listening on %s -> %s%s%s" % (a.listen, a.hub, " tls" if ctx else "",
                                        " prefix " + a.prefix if a.prefix else ""))
    while True:
        c, peer = ls.accept()
        if ctx:
            try:
                c = ctx.wrap_socket(c, server_side=True)
            except (ssl.SSLError, OSError) as e:
                log("TLS handshake from %s failed: %s" % (peer[0], e))
                c.close()
                continue
        log("connection from %s" % peer[0])
        threading.Thread(target=relay, args=(c, (hh, int(hp)), a.prefix), daemon=True).start()


if __name__ == "__main__":
    main()
