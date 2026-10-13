# vms-beszel: plan

## Context

[Beszel](https://github.com/henrygd/beszel) is a lightweight server monitor: a hub
(PocketBase plus a web UI) stores and charts statistics, raises alerts, and receives the
statistics from an agent on each monitored system. We want OpenVMS systems in the same
hub as everything else: real-time charts, history and alerts, with no change to the hub.

Only the **agent** is in scope. The hub already runs on the LAN (address in the git-ignored
`tools/hub.conf`) at version **0.21.0**, which is the version this port pins
(`upstream.conf`).

Goal: **100% compatibility with the unmodified 0.21.0 hub.** Defined as:

1. The hub accepts the agent, adds the system (normal token or universal token) and keeps
   it "up" across hub restarts, agent restarts and network drops.
2. Every hub feature that has meaning on OpenVMS works: the system list, the system page
   charts (CPU, memory, swap, disk usage and I/O, network, load average), the real-time
   (1 s) view, extra filesystems, and every alert type built on those statistics,
   including "system down".
3. Features with no OpenVMS counterpart (containers, GPU, temperatures, fans, battery,
   SMART, systemd, ZFS, Wi-Fi, package updates, journal logs, self-update) are reported
   as absent in the way upstream's agent reports them on a machine that lacks them. The
   hub must never log a protocol error because of us.
4. Compatibility is measured, not asserted: the conformance suite (D6) passes against the
   pinned upstream tree, and the live hub shows both nodes.

Name: **vms-beszel**, PCSI product **BESZEL_AGENT**, logical names and directories with
the prefix **BESZEL$** (D0).

## Why this is a re-implementation, not a patched port (D1)

The upstream agent is ~33,000 lines of Go, and Go does not exist for OpenVMS. Bringing up
the Go toolchain and runtime on VMS would dwarf this project. So, unlike the other vms-*
ports, nothing upstream is compiled. The pinned upstream tree is the **specification and
the test oracle**:

- the wire protocol comes from `internal/common/common-ws.go` and `internal/entities/**`
  (CBOR integer keys, `omitempty`/`omitzero` rules);
- the reference hub and upstream's own Go structs are used by the conformance tests;
- moving to a new upstream release means diffing those files between tags
  (`tools/protodiff.sh`), not rebasing patches.

## Decisions (details in DECISIONS.md)

- **D0 Names:** vms-beszel / BESZEL_AGENT / BESZEL$.
- **D1 Re-implementation in C:** Go isn't available on VMS (above).
- **D2 Language and ABI:** C99 and VSI C with 32-bit pointers, linking SSL3's `*_SHR32`
  images (the vms-steward/vmsguard combination). x86-64 first, then IA64.
- **D3 Transport: WebSocket only** (approved). The agent opens an outbound `ws://` or
  `wss://` connection to the hub. Upstream's other mode, where the hub opens an SSH
  connection to the agent on port 45876, is out of scope.
- **D4 WebSocket and TLS:** our own small RFC 6455 client on sockets plus SSL3, not
  libcurl.
- **D5 Configuration:** one file using upstream's variable names, with logical-name
  overrides.
- **D6 Compatibility testing:** upstream Go code checks our bytes. An upstream hub at the
  pinned tag runs on Linux for integration tests, and the LAN hub is used for the live
  tests.
- **D7 Reported version and unsupported actions:** we report the pinned upstream version
  and answer every hub action.
- **D8 Process model:** one single-threaded process with a `select()` loop, run detached
  under a dedicated unprivileged account.
- **D9 Metric mapping:** each beszel field gets a documented VMS source (table below).
  Values we derive are marked as derived; we never invent one.

## Architecture

```
            beszel hub 0.21.0 (unchanged)
                    ^
                    | ws[s]://hub/api/beszel/agent-connect   (outbound only)
                    | binary frames, CBOR (integer keys)
 BESZEL_AGENT.EXE --+----------------------------------------------------
   core/   cbor.c       CBOR encode/decode (subset: maps, ints, floats, strings, bytes, arrays)
           ws.c         RFC 6455 client framing, handshake, ping/pong, close
           proto.c      HubRequest/AgentResponse, action dispatch, fingerprint reply
           stats.c      Stats/Info/Details from raw counters: rates, deltas per cache
                        interval, percentages, load-average damping
           conf.c       config parsing and validation
           auth.c       hub signature check (Ed25519 over the token, via bz_crypto)
   plat/   bz_platform.h   the whole OS surface: sockets, TLS, crypto, time, raw counters,
                           files, logging; no VMS types
           vms/            $GETSYI/$GETRMI/$GETDVI/$QIO, TCP/IP Services, SSL3
           stub/           Linux: scripted counters + OpenSSL, for tests and conformance
```

- `core/` does no I/O and is unit-tested on Linux with `-std=c99 -pedantic -Wall -Wextra`.
- **The agent's state is the fingerprint.** It is a 48-hex-digit SHA-256 prefix, generated
  once and kept in `BESZEL$ROOT:[DATA]FINGERPRINT.`. The hub ties a system to it ("fingerprint
  mismatch" otherwise), so kit upgrades and removals must preserve it.
- **Delta tracking per cache interval:** the hub asks for data at its normal interval
  (60 s), and at 1 s in the real-time view, through `DataRequestOptions.CacheTimeMs`.
  Rates are computed per interval from separate baselines, as upstream's
  `deltatracker` does, so the real-time view doesn't skew the minute averages.

## Connection flow (as implemented by the 0.21.0 hub)

1. The agent sends a WebSocket upgrade to `<HUB_URL>/api/beszel/agent-connect` with the
   headers `X-Token: <TOKEN>` and `X-Beszel: 0.21.0`. The hub rejects an unknown token
   (401) or an unparsable version.
2. The hub sends `HubRequest{Action: CheckFingerprint, Data: {Signature, NeedSysInfo}}`.
   The signature is an SSH Ed25519 signature over the token, made with the hub's key.
   The agent verifies it against `KEY` (the hub's `ssh-ed25519` public key). Until a
   signature verifies, upstream's agent answers every other action with the error "hub
   not verified" (`agent/handlers.go`), and we do the same. This is what stops a rogue
   hub harvesting statistics.
3. The agent replies `FingerprintResponse{Fingerprint, [Hostname, Port, Name]}`. The hub
   binds the fingerprint to the system, or creates a system for a universal token.
4. Then requests arrive (`GetData` once per interval, and the others) with an `Id`, and
   each reply is an `AgentResponse{Id, Data | Error}`. The hub sends pings, and the
   connection times out after 120 s of silence.
5. On disconnect, the agent reconnects with backoff, and verifies the hub again.

## Metric mapping (first draft; superseded by D9 after the Phase 0 probes)

| beszel field | OpenVMS source | Notes |
|---|---|---|
| Cpu %, CpuBreakdown [user, system, iowait, steal, idle] | `$GETRMI` per-mode CPU ticks | user = user+super; system = kernel+exec+interrupt+MP sync; iowait = 0 (no VMS equivalent); steal = 0; to be decided in D9 |
| CpuCoresUsage | per-CPU mode ticks (`$GETRMI` modes or `$GETSYI` per-CPU) | probe |
| LoadAvg [1, 5, 15] | derived: exponentially damped compute-queue length, sampled every 5 s | VMS has no load average; documented as derived |
| Mem, MemUsed, MemPct, MemBuffCache | `$GETSYI` MEMSIZE, free/modified lists, page size; XFC size for cache | XFC interface: probe |
| Swap, SwapUsed | page and swap file totals and free | |
| DiskTotal/Used/Pct (root) | `$GETDVI` MAXBLOCK/FREEBLOCKS on SYS$SYSDEVICE (or FILESYSTEM) | |
| ExtraFs | the same, for each disk in EXTRA_FILESYSTEMS | |
| DiskReadPs/WritePs, DiskIO bytes | **gap**: `$GETDVI` OPCNT counts operations, not bytes | probe XFC per-volume statistics; otherwise D9 decides |
| NetworkSent/Recv, NetworkInterfaces, Bandwidth | LAN driver counters (`$QIO` sense-mode read counters, what LANCP uses) | privilege: probe |
| Uptime | SYI BOOTTIME | |
| Hostname, Kernel, CpuModel, Cores, Threads, Arch, MemoryTotal | SCSNODE / TCP/IP host name, VERSION, HW_NAME, CPU counts, ARCH_NAME | |
| Os, OsName | `Os = 0` (Linux) and `OsName = "OpenVMS <version>"` | the UI only knows 4 OSes and puts `osInfo[os]` straight into its info list, so an unknown value gives `undefined` there (D7) |
| Temperatures, Fans, GPU, Battery, SMART, Containers, systemd, ZFS, Wi-Fi, package updates | none | absent/empty, as upstream does on a machine without them |

## Phases

Each phase ends with a report (`docs/PHASE<n>.md`) and an exit review by the user.

### Phase 0: Specification and probes

- `tools/fetch.sh`: clone the pinned tag into `cache/` and refuse a commit mismatch.
- `docs/PROTOCOL.md`: every action, struct and CBOR key we send or accept, with each
  field's omit rule, from the pinned tree. Include what the hub does with errors, empty
  replies and missing fields for each action.
- Linux reference: build the upstream hub at the tag, and capture a session between it and
  the upstream agent (CBOR frames, decoded) as golden fixtures.
- VMS probes on **both** nodes (`probes/`, results in `docs/probes-<node>.txt`), with
  the privilege each needs:
  1. `$GETRMI` CPU mode ticks, system-wide and per CPU; compute-queue length.
  2. Memory lists, page size, page and swap file usage, XFC size.
  3. `$GETDVI` on every mounted disk; anything that gives I/O bytes (XFC volume stats).
  4. LAN counters for each LAN device, without privilege and with OPER/PHY_IO.
  5. SSL3: Ed25519 verify from `SSL3$LIBCRYPTO_SHR32`, TLS client, SHA-256.
  6. `select()` on a socket plus a TLS connection to the hub; check outbound reach to
     the hub.
- Exit: PROTOCOL.md complete; every metric row above resolved (source, privilege, or
  a decision to omit); D9 written.

### Phase 1: Core on Linux

- CBOR, WebSocket client, configuration, hub signature check, dispatcher, stats math, all
  unit-tested.
- The stub platform replays scripted counters.
- Conformance (D6):
  - a Go test in `tests/conformance/` decodes everything we emit into upstream's
    own structs, rejecting unknown fields and checking types;
  - our decoder is fed the hub's golden frames;
  - **integration:** the stub agent runs against a local upstream 0.21.0 hub, and a
    scripted check reads the records back through the hub's API.
- Exit: the local hub shows a stub system "up" with every chart filled, the real-time
  view works, and a CPU alert fires and resolves.

### Phase 2: VMS platform (x86-64, then IA64)

- `plat/vms/` from the probes; `BUILD.COM`; `tools/build.sh`.
- Every metric is cross-checked against MONITOR, SHOW MEMORY, SHOW DEVICE and LANCP at the
  same moment, with tolerances.
- Exit: both nodes show in the LAN hub with correct values, the run under the service
  account works with the minimum privileges, and the same conformance tests pass on the
  bytes from the VMS builds.

### Phase 3: Service and robustness

- Startup procedure (`SYS$STARTUP:BESZEL$STARTUP.COM`), detached process, restart on
  failure, a log file with versions kept, and a clean stop.
- Disconnection tests: hub restart, a network drop, a wrong `KEY` (must refuse), a wrong
  token, TLS with `CA_CERT_FILE` and with the kit's CA bundle, and the hub behind a reverse proxy.
- A 24-hour soak test on both nodes: memory flat, no handle leaks, gaps only where the
  hub was down.
- Security review: TLS checks, key handling, file protections, and fuzzing of the CBOR and
  WebSocket decoders.

### Phase 4: Kit and release

- PCSI kits for X86VMS and I64VMS, with an install procedure that creates the BESZEL
  account and the `BESZEL$ROOT` tree, installs a configuration template only if none
  exists, and never touches `[DATA]`.
- Install test on each node (system-wide: ask first), README, and release
  `v0.21.0-vms1` following the family process.

### Later (not scheduled)

- Network monitors (0.20+): ping/HTTP/TCP checks run from the VMS host.
- VMS "services" on the systemd panel: batch queues, or watched detached processes.
- SSH mode, if a user needs a hub-initiated connection.
- An upstream PR adding an OpenVMS entry to the hub's `Os` enum and icon.

## Risks

| Risk | Effect | Mitigation |
|---|---|---|
| No byte counters for disk I/O | the disk I/O charts are empty or approximate | Phase 0 probe 3; D9 records the choice |
| LAN counters need privilege | the agent account has more than read access | probe both ways; otherwise use TCP/IP Services counters |
| The hub's protocol changes in a later release | the agent drops behind | the reported version is pinned; `protodiff.sh` plus the conformance suite on every bump |
| An `Os` value the UI doesn't know | the system page breaks | always send `Os = 0` (D7) |
| The fingerprint changes (for example a reinstall wipes `[DATA]`) | the hub rejects the agent | the kit preserves `[DATA]`; documented recovery: reset the fingerprint in the hub |
| 32-bit `time_t` in VSI C | wrong after 2038 | use 64-bit VMS time for everything stored or sent |
