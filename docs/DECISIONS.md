# Decisions

Each decision with its status, the evidence and the alternatives considered.

## D0. Names: vms-beszel, BESZEL_AGENT, BESZEL$

**Status:** approved by the user (2026-10-13).

Repository `vms-beszel`, PCSI product `BESZEL_AGENT` (producer ISSINOHO), rooted logical
`BESZEL$ROOT`, image `[BIN]BESZEL_AGENT.EXE`. C identifiers use the prefix `bz_`.

## D1. Re-implement the agent in C; the upstream tree is the specification

**Status:** proposed (2026-10-13).

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

**Status:** proposed (2026-10-13).

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

**Status:** proposed (2026-10-13).

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

**Status:** proposed (2026-10-13).

`BESZEL$ROOT:[CONF]AGENT.CONF`, one `NAME=value` per line, `#` comments. The names are
upstream's environment variables, so beszel's documentation and the hub's "add system"
dialog apply directly: `HUB_URL`, `TOKEN` or `TOKEN_FILE`, `KEY` or `KEY_FILE`,
`CA_CERT_FILE`, `HUB_HEADERS`, `SYSTEM_NAME`, `FILESYSTEM`, `EXTRA_FILESYSTEMS`, `NICS`,
`LOG_LEVEL`, `DATA_DIR`. A logical name `BESZEL$<NAME>` (process, then system table)
overrides a file entry, as upstream's `BESZEL_AGENT_<NAME>` environment variables do.
The file holds the token, a secret, so the kit creates it as
`(S:RWED,O:RWED,G,W)` owned by the BESZEL account, and the agent warns at startup if it
is world- or group-readable. Unknown names are an error, so a typo doesn't fail
silently.

## D6. Conformance tested against upstream's own code

**Status:** proposed (2026-10-13).

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

**Status:** proposed (2026-10-13). To be confirmed against the hub in Phase 1.

`X-Beszel` and `Info.AgentVersion` are `0.21.0`, the pinned version, so the hub uses
the current protocol (an `AgentResponse` wrapper and generic `Data` payloads) and
doesn't flag the agent as outdated. Every action gets an answer: `GetData` in full;
`CheckFingerprint` in full; `GetSmartData` an empty map; `SyncNetworkMonitors` an empty
`SyncResponse`, or an error, whichever the hub handles silently (Phase 0 decides); the
container, systemd, ZFS and package-update actions an `Error` naming the feature as
unsupported on OpenVMS. `Os` is always 0 (Linux), with `OsName` "OpenVMS E9.2-4",
because the 0.21.0 UI knows only four values (`info-bar.tsx`: `osInfo[os]` is used
without a fallback).

Alternatives: report 0.19.x to keep the hub from sending network-monitor syncs (it gates
them on 0.20.0). Rejected unless Phase 0 shows an error reply makes the hub misbehave.

## D8. One single-threaded process, detached, unprivileged account

**Status:** proposed (2026-10-13).

VMS has no `fork()`, and the work is light: one socket, one timer for the 5-second
load-average sample, and collection when a request arrives. A `select()` loop over the
TCP/IP Services socket with SSL3 non-blocking I/O is enough, with no threads. It runs
as `RUN/DETACHED` (through LOGINOUT, so it gets a process log) under a BESZEL account
with only the privileges the Phase 0 probes prove necessary.

## D9. Metric mapping

**Status:** open. Phase 0 decides each row of the table in PLAN.md.

Principles: a value comes from a documented system interface or is omitted. Derived
values (load average) are computed the way Linux defines them and documented as derived.
No field is filled with a guess to make a chart look populated.
