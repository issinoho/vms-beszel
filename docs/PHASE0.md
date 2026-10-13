# Phase 0: specification and probes (report)

**Status:** done (2026-10-13), awaiting the user's exit review.

**Verdict: feasible, and no blocker found.** Everything the agent needs works on both nodes
from an unprivileged process:
- the protocol (WebSocket, CBOR, the Ed25519 check of the hub);
- TLS;
- the system counters behind every core chart.

There is one real gap: disk I/O, which has no byte counters on VMS. One risk needs a Phase 2
check: network counters that may stop at 4 GiB.

## Exit criteria

| Criterion (PLAN.md) | Result |
|---|---|
| `docs/PROTOCOL.md` complete | Done. 9 sections, every fact cited `file:line` in the pinned tree, and section 9 checks it against a live capture. 12 open questions: 2 settled by the capture (Q2, Q6), 2 answered by D9 and D5 (Q7, Q12), 5 to be settled by Phase 1 tests, and 3 decisions listed below (Q9–Q11). |
| Every metric row resolved: source, privilege, or a decision to omit | Done. D9 proposes each row; two items stay open for Phase 2 (XFC size, LAN counter width). |
| D9 written | Proposed, for review. |

## What was done

1. **Pinned upstream.** `tools/fetch.sh` clones v0.21.0 and checks the commit (`ec96c13`);
   `--ref` also fetches the official release binaries and checks their checksums. The LAN
   hub reports `HUB_VERSION 0.21.0` in its page, so this is the exact version it runs.
2. **Protocol specification**: `docs/PROTOCOL.md`, written from the source.
3. **Reference capture.** The upstream hub and agent ran on Linux through a logging proxy.
   That gave 56 frames over 200 s: handshake, details, network-monitor sync, SMART
   request, 20 s of real time, and 60 s ticks (PROTOCOL.md 9). Tools: `refhub.sh`,
   `wscap.py`, `wsdecode.py`, `rtsub.py`. The frames stay in `out/` (D10).
4. **Hub signature check.** On Linux, `openssl pkeyutl` verified the captured signature as
   raw Ed25519 over the token bytes, and rejected a modified token.
5. **VMS probes** on both nodes, each run with only NETMBX and TMPMBX and again with all
   privileges (`probes/`, findings in `docs/NOTES.md`):
   - `probe_stats`: `$GETSYI`, `$GETDVI` disks, LAN counters, and cross-checks with
     SHOW MEMORY, MONITOR MODES, LANCP and TCPIP.
   - `probe_rmi`: how `$GETRMI` wants its buffers, and the `RMI$_MODES` layout.
   - `probe_tls`: Ed25519 (RFC 8032), SHA-256, non-blocking sockets with `select()`, a
     WebSocket upgrade to the LAN hub (a made-up token, answered 401 as expected), and a
     TLS 1.3 handshake.

## Key findings

**Protocol** (details in PROTOCOL.md):
- Plain RFC 6455: no compression, no subprotocol, binary frames only. The hub's 60 s
  `GetData` is the keep-alive.
- **0.21.0 still replies in the legacy fields** for the handshake (key 2) and the data
  (key 1). Only newer actions use the generic `Data` (key 7).
- **fxamacker's encoding rules are part of the contract:**
  - floats are always 8-byte;
  - `[]uint8` and `[2]uint8` are byte strings;
  - `Containers` is always `null`;
  - `omitempty` does not omit a struct-typed field.

  The D6 conformance tests must pin all of these.
- The hub sends three requests on its own (SMART, ZFS, network-monitor sync). Each has a
  silent reply, listed in PROTOCOL.md 3.3. **`GetData` must never answer with an error:**
  the hub then drops the connection and marks the system down.
- **What could break the UI**, all avoided by D7/D9:
  - an `Os` outside 0–3;
  - sending Info `m` or `h`;
  - an extra filesystem with size 0, which fires the disk alert at once.

**OpenVMS** (details in NOTES.md):
- **No privilege is needed** for anything the agent reads. The service account can be a
  plain account with NETMBX and TMPMBX.
- `$GETRMI` takes **exact item sizes only**. `RMI$_MODES` gives per-CPU mode times. We
  decoded its layout and checked it against MONITOR and the system-wide items. Its return
  length is wrong on both architectures, in different ways.
- **Load average** can be derived properly from `RMI$_CUR + COM + COMO`.
- **Disks report operations, not bytes.** D9 proposes not sending disk I/O rather than
  inventing bytes.
- **LAN counters are readable unprivileged but 32-bit.** The IA64 node's busiest NIC is 3.9
  GB into a 4.29 GB counter. LANCP shows a wider value for a related counter, so a wider
  interface exists.
- The XFC size isn't available through `$GETRMI` (it returns 0 even with privilege).
- SSL3 (OpenSSL 3.0.21 and 3.0.22) does everything we need. It has no CA store, so the kit
  ships a CA bundle for `https` hubs (D5).

## Decisions for the exit review

| # | Question | Recommendation |
|---|---|---|
| D9 | The metric mapping, in particular: no disk I/O charts; iowait and steal reported as 0; load average derived from the queues | Accept as proposed. |
| D5 | Drop `HUB_HEADERS` (not in 0.21.0); ship a CA bundle; never skip TLS verification | Accept. |
| D7 | The reply to each hub action as in PROTOCOL.md 3.3, mirroring upstream's bytes | Accept. |
| D10 | Upstream release binaries as the reference; captures never committed | Accept. |
| PROTOCOL Q9 | The hub lets users create network monitors (ping/HTTP checks) for our system, which will show no data until we implement them | Accept for v1; keep network monitors on the "later" list. |
| PROTOCOL Q10 | Send a long `SmartInterval` to space out the hub's hourly "SMART fetch" info line? | No: keep upstream's behaviour. It is one info line an hour, the same as for a Linux host without disks that report SMART. |
| PROTOCOL Q11 | The OS text: "OpenVMS E9.2-4 (x86-64)" next to the Tux icon, with "OpenVMS E9.2-4" as the tooltip | Accept, or suggest wording. |

## Carried forward

- **Phase 1** (tests settle these): PROTOCOL Q1, Q3, Q4, Q5, Q8. These are gws and
  fxamacker behaviour, semver strictness, and the ZFS reply keys; all can be tested against
  the reference hub and upstream's structs.
- **Phase 2:**
  - Watch IA64 EWB0's counter pass 2^32 (wrap or latch?), and find LANCP's wider counter
    interface.
  - Find the XFC size interface.
  - Decide whether "swap used" means page-file pages used or committed.
- **Housekeeping:** the private reference hub is stopped at the end of this phase. Its data
  is in `out/refhub`.
