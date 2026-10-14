# Decisions

Each decision with its status, the evidence and the alternatives considered.

## D0. Names: vms-beszel, BESZEL_AGENT, BESZEL$

**Status:** approved by the user (2026-10-13).

Repository `vms-beszel`, PCSI product `BESZEL_AGENT` (producer ISSINOHO), rooted logical
`BESZEL$ROOT`, image `[BIN]BESZEL_AGENT.EXE`. C identifiers use the prefix `bz_`.

## D1. Re-implement the agent in C; the upstream tree is the specification

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

Upstream's agent is Go (beszel 0.21.0: ~33,000 lines in `agent/`, plus gopsutil and
other dependencies). There is no Go toolchain or runtime for OpenVMS. The family's usual
model (a pinned upstream tarball, `patches/`, `overlay/`) therefore cannot apply.
Instead:

- `upstream.conf` pins the tag and the commit, and `tools/fetch.sh` checks both;
- the wire structs and the hub's behaviour at that tag are the specification
  (`docs/PROTOCOL.md`, written in Phase 0 with file and line references);
- upstream's Go structs and hub are the test oracle (D6);
- an upstream bump is a protocol diff between tags (`tools/protodiff.sh`), followed by
  code changes and the conformance suite.

Alternatives:
- **Port Go to OpenVMS:** a new GOOS, the runtime, syscalls, an ELF/VMS linker, and
  system-service calls through cgo. Years, not months.
- **An agent on Linux that polls VMS over SSH or SNMP:** not native, an extra machine
  per site, and SNMP's VMS MIB is thin. Rejected by the goal.
- **A Python agent on VSI Python:** an interpreter dependency on every node, no
  CBOR/WebSocket libraries packaged for VMS, and system-service access through C
  extensions anyway.

## D2. C99, VSI C, 32-bit pointers, SSL3 *_SHR32

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

The same combination as vms-steward and vmsguard: system-service item lists are 32-bit
structures, and SSL3's `*_SHR32` images match. VSI C exists on both nodes (x86-64
V7.7-003, IA64 V7.4-001). C99 only: no VLAs, no anonymous unions, nothing C11. `uint64_t`
and IEEE `double` are required (CBOR float64); both are the defaults on x86-64 and IA64.

## D3. WebSocket transport only

**Status:** approved by the user (2026-10-13).

The agent connects out to `<HUB_URL>/api/beszel/agent-connect`. This is the default for
systems added in the hub with a token, needs no listening port on the VMS host, and goes
through firewalls and reverse proxies. Upstream's SSH mode (the hub connects to the
agent on 45876 with curve25519-sha256, chacha20-poly1305 and hmac-sha2-256-etm) would
need an SSH server implementation or a libssh port. It goes on the "later" list.

## D4. Our own WebSocket client on SSL3, not libcurl

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

The client side of RFC 6455 is small: the upgrade request, the `Sec-WebSocket-Accept`
check, masked frames out, unmasked frames in, ping/pong, close, and fragmentation. That
is roughly 500 lines, unit-testable in `core/` and driven by one `select()` loop with TLS
from SSL3.

Alternative: vms-curl 8.22 (whose build lists `ws`/`wss`) with `CURLOPT_CONNECT_ONLY=2`
and `curl_ws_send`/`curl_ws_recv`. Rejected for v1: it adds a kit prerequisite,
libcurl's WebSocket API is untested on VMS, and it is harder to merge into a single
event loop. Revisit if we need `BESZEL_AGENT_ALL_PROXY` (SOCKS/HTTP proxies), which
libcurl would provide.

## D5. Configuration file with upstream's names

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

`BESZEL$ROOT:[CONF]AGENT.CONF`, one `NAME=value` per line, `#` comments. The names are
upstream's environment variables, so beszel's documentation and the hub's "add system"
dialog apply directly: `HUB_URL`, `TOKEN` or `TOKEN_FILE`, `KEY` or `KEY_FILE`,
`CA_CERT_FILE`, `SYSTEM_NAME`, `FILESYSTEM`, `EXTRA_FILESYSTEMS`, `NICS`, `LOG_LEVEL`,
`DATA_DIR`. Parsing follows PROTOCOL.md section 7.

*Phase 0 revision:* `HUB_HEADERS` is dropped. It is not in 0.21.0 (PROTOCOL.md 1.2; I had
read it from upstream's development branch), and the agent sends exactly upstream's three
headers. It comes back when an upstream release with it is pinned. SSL3 loads no CA store
for us (NOTES.md 20): with `https` hubs the agent verifies against `CA_CERT_FILE` if set,
and otherwise against a CA bundle shipped in the kit (the same curl.se bundle vms-curl
ships). It never skips verification. A logical name `BESZEL$<NAME>` (process, then system table)
overrides a file entry, as upstream's `BESZEL_AGENT_<NAME>` environment variables do.
The file holds the token, a secret, so the kit creates it as
`(S:RWED,O:RWED,G,W)` owned by the BESZEL account, and the agent warns at startup if it
is world- or group-readable. Unknown names are an error, so a typo doesn't fail
silently.

## D6. Conformance tested against upstream's own code

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

1. **Encoding:** a Go test module in `tests/conformance/` imports the pinned
   upstream packages (`internal/common`, `internal/entities/...`) and decodes every
   message we emit (from fixtures produced by the Linux stub build) with
   `cbor.DecOptions{ExtraReturnErrors: ExtraDecErrorUnknownField}`. It checks that types
   and omissions round-trip.
2. **Decoding:** CBOR frames captured from the upstream hub talking to the upstream
   agent are replayed into our decoder.
3. **Integration:** the upstream hub, built from the tag on Linux, runs with the stub
   agent; a script checks the system's records through the hub's API.
4. **Live:** the VMS builds against the LAN hub (0.21.0).

Go is needed on the Linux workstation only.

## D7. Report the pinned upstream version; answer every action

**Status:** approved by the user at the Phase 0 exit (2026-10-14); backed by the Phase 0
capture (PROTOCOL.md 3.3, 9). Final confirmation is the Phase 1 integration test.

`X-Beszel` and `Info.AgentVersion` are `0.21.0`, the pinned version, so the hub uses
the current protocol (an `AgentResponse` wrapper and generic `Data` payloads) and
doesn't flag the agent as outdated. Each action gets the reply in PROTOCOL.md 3.3, which
mirrors what upstream sends on a bare host wherever that is silent on the hub:
- `GetData`: always data, never `Error`. An error makes the hub drop the WebSocket and mark
  the system down.
- `CheckFingerprint`: legacy key 2.
- `GetSmartData`: `{0:Id}`, as upstream without smartctl (captured).
- `SyncNetworkMonitors`: upstream's exact zero `SyncResponse`,
  `{0:Id, 7:{0:{8:0,10:0,11:0,12:0}}}` (captured). An error here would make the hub log
  a warning every minute.
- `GetZfsData`: upstream's "complete, no pools" reply.
- Container, systemd and package actions (which the UI never sends for a system that
  reports none of them): the replies in 3.3. `Os` is always 0 (Linux), with `OsName` "OpenVMS E9.2-4",
because the 0.21.0 UI knows only four values (`info-bar.tsx`: `osInfo[os]` is used
without a fallback).

Alternatives: report 0.19.x to keep the hub from sending network-monitor syncs (it gates
them on 0.20.0). Rejected unless Phase 0 shows an error reply makes the hub misbehave.

## D8. One single-threaded process, detached, unprivileged account

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

VMS has no `fork()`, and the work is light: one socket, one timer for the 5-second
load-average sample, and collection when a request arrives. A `select()` loop over the
TCP/IP Services socket with SSL3 non-blocking I/O is enough, with no threads. It runs
as `RUN/DETACHED` (through LOGINOUT, so it gets a process log) under a BESZEL account
with only the privileges the Phase 0 probes prove necessary.

## D9. Metric mapping

**Status:** approved by the user at the Phase 0 exit (2026-10-14), from the Phase 0 probes
(NOTES.md). The open items (XFC size, LAN counter width) are settled in Phase 2.

Principles: a value comes from a documented system interface or is left out. Derived
values (load average) are computed the way Linux defines them and documented as derived.
No field is filled with a guess to make a chart look populated. Units are upstream's
(PROTOCOL.md 4.4–4.7). Everything below works without privilege, on both nodes.

| beszel field | Source | Rule |
|---|---|---|
| Cpu, per-core `cpus` | `RMI$_MODES`, per CPU, deltas over the request's interval | total = `[0]..[6]`, idle = `[7]`; busy = total − idle |
| `cpub` [user, system, iowait, steal, idle] | the same | user = user + super; system = kernel + exec + MP synch + interrupt (`[0]−[7]`, clamped at 0) + compat; **iowait = 0**, **steal = 0** (VMS doesn't measure them); idle. "Other" is then 0. |
| LoadAvg [1, 5, 15] | `RMI$_CUR + RMI$_COM + RMI$_COMO`, sampled every 5 s | Linux's exponential damping (constants 1884/2014/2037 over 2048, as `calc_load`). **Derived**, and documented as such. It starts at 0 at agent start, as Linux does at boot. |
| Mem, MemUsed, MemPct | `SYI$_MEMSIZE` × `SYI$_PAGE_SIZE`; free = `RMI$_FRLIST`; used = total − free − `RMI$_MODLIST` | The modified list is reclaimable cache, like Linux's dirty page cache. |
| MemBuffCache | XFC allocated size | **Open:** `RMI$_CACHE_MEMORY` returns 0. Phase 2 looks for the XFC interface; until then send 0 (the field is required, and 0 is valid). |
| Swap, SwapUsed | the page files plus the swap files, from `SYI$_PAGEFILE_*` and `SYI$_SWAPFILE_*` | used = page − free. The chart shows only when used > 0 (PROTOCOL.md 4.8). |
| DiskTotal, DiskUsed, DiskPct (root) | `$GETDVI` MAXBLOCK, FREEBLOCKS on `SYS$SYSDEVICE` (or `FILESYSTEM`) | 512-byte blocks; pct = used / total (no root reserve on ODS-5, so gopsutil's used/(used+avail) is the same) |
| ExtraFs | the same, for each disk in `EXTRA_FILESYSTEMS` | Upstream's syntax, with VMS device names (`DKA200:__tools`). |
| **Disk I/O** (`dio`, `dr`, `dw`, `diot`, `dios`) | `DVI$_OPCNT`: operation counts only | **Not sent.** There is no byte counter (NOTES.md 9), and operations × a guessed size would be invented data. The UI then hides the disk I/O chart, as it does for upstream on a machine without I/O counters. Revisit if Phase 2 finds per-volume XFC byte statistics. |
| Bandwidth, `ni` | LAN device counters (`$QIO` read-counters, unit 0) | B/s from deltas; per NIC as upstream. Devices with no traffic are skipped unless named in `NICS`. **Open:** 32-bit counters that may latch (NOTES.md 13); Phase 2 checks, and finds LANCP's wider interface if needed. |
| Uptime | `SYI$_BOOTTIME` and the current time | seconds |
| Details.Hostname | TCP/IP host name (`gethostname`), else `SYI$_NODENAME` | |
| Details.Kernel | `"OpenVMS " + SYI$_VERSION` (`"OpenVMS E9.2-4"`) | the tooltip |
| Details.OsName | `"OpenVMS " + SYI$_VERSION`, plus `" (x86-64)"`/`" (Integrity)"` | the visible text, next to the Tux icon (D7) |
| Details.CpuModel | `SYI$_HW_NAME` | "QEMU Standard PC (Q35 + ICH9, 2009)", "HP rx2660 (1.59GHz/6.0MB)" |
| Details.Cores, Threads, Info.Threads | `SYI$_ACTIVECPU_CNT` | VMS doesn't report cores and threads separately |
| Details.Arch | `SYI$_ARCH_NAME` | "x86_64", "IA64" |
| Details.MemoryTotal | MEMSIZE × PAGE_SIZE | bytes |
| Fingerprint | SHA-256 of the 32 hex digits of `SYI$_SYSTEM_UUID` (as `F$GETSYI` prints it), first 24 bytes as hex; if the UUID is all zeros, of hostname + CPU model, as upstream. Saved in `[DATA]FINGERPRINT.` | upstream's `generateFingerprint`, with the UUID standing in for `host.HostID()` |
| Temperatures, fans, GPU, batteries, Wi-Fi, ZFS, containers, systemd, package updates | none | absent, exactly as upstream on a machine without them (Containers is always `null`, Monitors always `{}`) |

## D10. Reference tooling and golden fixtures

**Status:** approved by the user at the Phase 0 exit (2026-10-14).

The reference is upstream's **release binaries** (hub and agent, linux/amd64, verified
against the release checksums), not a source build: they are what users run, and building
the hub would need the web UI toolchain. `tools/fetch.sh --ref` fetches them into `cache/ref/`.
`tools/refhub.sh` runs a private hub on 127.0.0.1:18090. `tools/wscap.py`, `wsdecode.py` and
`rtsub.py` capture and decode sessions. Go 1.27.2 (for the D6 conformance tests) is
unpacked in `cache/toolchain/`, verified against go.dev's SHA-256.

Captures of the upstream agent hold the workstation's real data (sensors, services, SSID),
so they stay in `out/` and are **never committed**. Committed fixtures are produced by our
stub platform with synthetic data, and checked against upstream's structs (D6).
