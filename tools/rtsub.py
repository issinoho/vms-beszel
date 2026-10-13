#!/usr/bin/env python3
"""rtsub.py - hold a hub real-time subscription open, as the system page does.

    rtsub.py <hub-base-url> <auth-token> <system-id> <seconds>

While subscribed to rt_metrics for the system, the hub asks that agent for data
every second (DataRequestOptions.CacheTimeMs = 1000).
"""
import json
import sys
import threading
import time
import urllib.request

base, token, system, secs = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
resp = urllib.request.urlopen(base + "/api/realtime", timeout=secs + 30)
client_id = None
while client_id is None:
    line = resp.readline().decode()
    if line.startswith("data:"):
        client_id = json.loads(line[5:])["clientId"]
topic = "rt_metrics?options=" + urllib.request.quote(json.dumps({"query": {"system": system}}))
req = urllib.request.Request(base + "/api/realtime", method="POST",
                             data=json.dumps({"clientId": client_id, "subscriptions": [topic]}).encode(),
                             headers={"Content-Type": "application/json", "Authorization": token})
urllib.request.urlopen(req, timeout=10).read()
n = 0
end = time.time() + secs


def reader():
    global n
    for raw in resp:
        if raw.startswith(b"data:"):
            n += 1


threading.Thread(target=reader, daemon=True).start()
while time.time() < end:
    time.sleep(0.5)
print("rtsub: %d events in %.0f s" % (n, secs))
