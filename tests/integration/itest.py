#!/usr/bin/env python3
"""itest.py - the Phase 1 exit test: our agent against upstream's own hub.

    tests/integration/itest.py [agent-binary]      (default build/beszel_agent-asan)

Runs against the private reference hub (tools/refhub.sh, upstream's 0.21.1
release binary) and checks, with a fresh system each run:

  1. the hub accepts the agent and marks the system "up";
  2. system_details and info hold what the stub platform reports;
  3. the real-time view gets about one reply a second;
  4. a CPU alert fires when load rises above its threshold and resolves after;
  5. after the agent restarts it reconnects to the same system;
  6. with a wrong KEY the agent refuses the hub and answers nothing;
  7. the sanitizer build reports nothing.

Only processes this script starts are stopped, by PID.  Takes about 8 minutes.
"""
import json
import os
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REFHUB = os.path.join(ROOT, "tools", "refhub.sh")
BASE = "http://127.0.0.1:%s" % os.environ.get("REFHUB_PORT", "18090")
AGENT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "beszel_agent-asan")
WORK = os.path.join(ROOT, "out", "itest")

failures = []


def step(msg):
    print("== %s" % msg, flush=True)


def check(ok, what):
    print("   %s %s" % ("ok  " if ok else "FAIL", what), flush=True)
    if not ok:
        failures.append(what)
    return ok


def refhub(*args):
    return subprocess.run([REFHUB] + list(args), check=True, capture_output=True, text=True).stdout


def api(path):
    return json.loads(refhub("api", path))


def system(sid):
    return api("/api/collections/systems/records/%s" % sid)


def alert_triggered(aid):
    # alerts can be listed by their owner but viewed by id only by superusers
    items = api('/api/collections/alerts/records?filter=(id="%s")' % aid)["items"]
    return bool(items and items[0]["triggered"])


def wait(pred, timeout, every=2):
    end = time.time() + timeout
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(every)
    return None


class Agent:
    def __init__(self, conf, log):
        self.log = log
        env = dict(os.environ, BZ_STUB_CONTROL=os.path.join(WORK, "control"),
                   ASAN_OPTIONS="detect_leaks=1:abort_on_error=0",
                   UBSAN_OPTIONS="print_stacktrace=1")
        self.f = open(log, "w")
        self.p = subprocess.Popen([AGENT, conf], stdout=self.f, stderr=subprocess.STDOUT, env=env)

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.f.close()
        return self.p.returncode

    def text(self):
        with open(self.log) as f:
            return f.read()


def control(cpu):
    with open(os.path.join(WORK, "control"), "w") as f:
        f.write("cpu %d\n" % cpu)


def write_conf(path, env, key=None):
    with open(path, "w") as f:
        f.write("HUB_URL=%s\nTOKEN=%s\nKEY=\"%s\"\nDATA_DIR=%s\n"
                "EXTRA_FILESYSTEMS=DKA200:__Tools\nFILESYSTEM=DKA0:__System\nLOG_LEVEL=debug\n"
                % (BASE, env["TOKEN"], key or env["KEY"], os.path.join(WORK, "data")))
    os.chmod(path, 0o600)


def main():
    os.makedirs(os.path.join(WORK, "data"), exist_ok=True)
    for f in os.listdir(os.path.join(WORK, "data")):
        os.remove(os.path.join(WORK, "data", f))
    refhub("start")
    name = "itest-%d" % int(time.time())
    env = {}
    for line in refhub("init", name).splitlines():
        k, v = line.split("=", 1)
        env[k] = v.strip("'")
    sid = env["SYSTEM_ID"]
    conf = os.path.join(WORK, "agent.conf")
    write_conf(conf, env)
    control(20)
    agents = []
    try:
        step("1. connect: the hub marks the system up (first update within ~60 s)")
        a = Agent(conf, os.path.join(WORK, "agent1.log"))
        agents.append(a)
        rec = wait(lambda: (lambda r: r if r["status"] == "up" else None)(system(sid)), 90)
        check(rec is not None, "system %s is up" % name)

        step("2. details and info")
        det = wait(lambda: (lambda d: d["items"][0] if d["items"] else None)(
            api('/api/collections/system_details/records?filter=(system="%s")' % sid)), 30)
        check(det is not None, "system_details record exists")
        if det:
            check(det["os"] == 0 and det["os_name"] == "OpenVMS E9.2-4 (x86-64)", "OS reported as Linux/\"OpenVMS ...\"")
            check(det["hostname"] == "stubvms" and det["cores"] == 2 and det["memory"] == 8 << 30, "hostname, cores, memory")
        rec = system(sid)
        info = rec.get("info") or {}
        check(info.get("v") == "0.21.1" and info.get("ct") == 2, "agent version 0.21.1 over WebSocket")
        check("Tools" in (info.get("efs") or {}), "extra filesystem Tools reported in info")
        # 0.21.1's byte fields: the hub stores ours as sent.  Had it converted the GB
        # fields instead, 128 MiB of swap (0.13 GB) would come back as 139586437.
        st = wait(lambda: (lambda r: r["items"][0]["stats"] if r["items"] else None)(
            api('/api/collections/system_stats/records?filter=(system="%s")&sort=-created' % sid)), 90)
        check(st is not None and st.get("swap") == [2 << 30, 128 << 20], "swap bytes stored as sent")
        check(st is not None and (st.get("mem") or [0])[0] == 8 << 30 and len(st.get("mem")) == 4,
              "memory bytes stored as sent")

        step("3. real-time view: ~1 reply a second for 10 s")
        tok = refhub("token").strip()
        out = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "rtsub.py"), BASE, tok, sid, "10"],
                             capture_output=True, text=True).stdout
        n = int(out.split()[1]) if out.startswith("rtsub:") else 0
        check(n >= 8, "%d real-time events in 10 s" % n)

        step("4. CPU alert: fires above 80%, resolves below")
        uid = refhub("uid").strip()
        alert = json.loads(refhub("post", "/api/collections/alerts/records",
                                  json.dumps({"user": uid, "system": sid, "name": "CPU", "value": 80, "min": 1})))
        control(95)
        fired = wait(lambda: alert_triggered(alert["id"]), 200, 5)
        check(bool(fired), "alert triggered at 95% CPU")
        control(5)
        cleared = wait(lambda: not alert_triggered(alert["id"]), 200, 5)
        check(bool(cleared), "alert resolved at 5% CPU")
        hist = api('/api/collections/alerts_history/records?filter=(alert_id="%s")' % alert["id"])["items"]
        check(len(hist) == 1 and hist[0].get("resolved"), "one alerts_history entry, resolved")

        step("5. restart: the same system reconnects (fingerprint kept)")
        fp1 = open(os.path.join(WORK, "data", "fingerprint")).read().strip()
        check(a.stop() == 0, "agent stopped cleanly on SIGTERM")
        down = wait(lambda: system(sid)["status"] == "down", 30)
        check(bool(down), "hub marked the system down")
        a2 = Agent(conf, os.path.join(WORK, "agent2.log"))
        agents.append(a2)
        up = wait(lambda: system(sid)["status"] == "up", 90)
        check(bool(up), "system up again after the restart")
        fp2 = open(os.path.join(WORK, "data", "fingerprint")).read().strip()
        check(fp1 == fp2 and len(fp1) == 48, "fingerprint unchanged (%s)" % fp1[:12])
        check(a2.stop() == 0, "second agent stopped cleanly")

        step("6. wrong KEY: the hub's signature is refused, nothing answered")
        wrong = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGfBvEpvPfz6WUCsUZhzeAWoLgYdyD5KjxtFmJ6AgdT2"
        bad = os.path.join(WORK, "agent-badkey.conf")
        write_conf(bad, env, key=wrong)
        a3 = Agent(bad, os.path.join(WORK, "agent3.log"))
        agents.append(a3)
        time.sleep(20)
        a3.stop()
        t3 = a3.text()
        check("invalid signature - check KEY value" in t3, "agent logged the invalid signature")
        check("WebSocket connected" not in t3, "agent never considered the hub verified")
        check(system(sid)["status"] != "up", "system not marked up")

        step("7. sanitizers")
        for ag in agents:
            t = ag.text()
            bad_san = [l for l in t.splitlines() if "ERROR: AddressSanitizer" in l or "runtime error" in l
                       or "LeakSanitizer" in l]
            check(not bad_san, "%s: no sanitizer reports" % os.path.basename(ag.log))
    finally:
        for ag in agents:
            ag.stop()
    print("\n%s: %d failure(s)" % ("PASS" if not failures else "FAIL", len(failures)))
    for f in failures:
        print("  - " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
