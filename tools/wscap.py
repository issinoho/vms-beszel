#!/usr/bin/env python3
"""wscap.py - a logging TCP proxy for capturing agent <-> hub WebSocket traffic.

    wscap.py <listen-port> <hub-host:port> <capture-file>

Each accepted connection is relayed to the hub and every chunk is appended to
<capture-file> as one JSON line: {"c": conn, "t": seconds, "d": "a2h"|"h2a", "x": hex}.
Decode with wsdecode.py.  Plain TCP only (point the agent at ws://, not wss://).
"""
import json
import select
import socket
import sys
import threading
import time

lock = threading.Lock()


def relay(conn_id, agent, hub, out, t0):
    peers = {agent: (hub, "a2h"), hub: (agent, "h2a")}
    try:
        while True:
            ready, _, _ = select.select(list(peers), [], [])
            for s in ready:
                data = s.recv(65536)
                if not data:
                    return
                dst, direction = peers[s]
                dst.sendall(data)
                with lock:
                    out.write(json.dumps({"c": conn_id, "t": round(time.time() - t0, 3),
                                          "d": direction, "x": data.hex()}) + "\n")
                    out.flush()
    except OSError:
        pass
    finally:
        agent.close()
        hub.close()


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    port = int(sys.argv[1])
    host, hport = sys.argv[2].rsplit(":", 1)
    out = open(sys.argv[3], "a")
    t0 = time.time()
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(4)
    n = 0
    while True:
        agent, _ = ls.accept()
        n += 1
        hub = socket.create_connection((host, int(hport)))
        threading.Thread(target=relay, args=(n, agent, hub, out, t0), daemon=True).start()


if __name__ == "__main__":
    main()
