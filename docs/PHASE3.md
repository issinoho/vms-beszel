# Phase 3: service and robustness (report)

**Status:** done (2026-10-15). Exit review: approved by the user on 2026-10-15, including D12–D15.

**Verdict:** the agent runs as an unprivileged service on both architectures and survived
24 hours against the user's real hub with no restart and no growth in memory. That
included the hub's nightly backup and a real silent network drop. The security review's
findings are fixed. One Phase 2 question was settled the hard way: the LAN counters stick at
4 GiB, so stuck devices are now read through LANCP (D15).

## The service

| Piece | What it does |
|---|---|
| `vms/BESZEL$STARTUP.COM START\|STOP\|RESTART\|STATUS` | START runs `BESZEL$RUN.COM` as the detached process `BESZEL_AGENT` through LOGINOUT, with its log in `BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG` (5 versions kept). If `BESZEL$USER` names an account, it runs as that account. STOP creates `[DATA]STOP.`, waits up to 20 s for a clean exit, and only then uses STOP/ID. |
| `vms/BESZEL$RUN.COM` | Drops to NETMBX and TMPMBX, finds `BESZEL$ROOT` from its own location when no logical name exists, and restarts the agent after a failure. The wait starts at 15 s and doubles to 5 minutes; a run of 10 minutes or more resets it. |
| The agent | Exits cleanly within 2 s of the stop file appearing (with a close frame, so the hub marks the system down at once), and removes a stale stop file at start |
| `vms/agent.conf_template` | Every setting, explained, in upstream's names |

No privileges, mailboxes or system logical names are involved. The kit (Phase 4) adds the
account, the system logical names and the SYS$STARTUP entry.

## Tests

Run on x86 against the reference hub, with a tree laid out as the kit will lay it out
(`tools/vmstree.sh`), through `tools/testproxy.py` where a network fault or TLS was needed:

| Test | Result |
|---|---|
| Start and status | Detached, unprivileged (`TMPMBX,NETMBX`), connected |
| Stop | The agent exits with status 1 within 2 s; the run loop ends; the hub marks the system down |
| A broken config | Each failure logged with its line; waits of 15, 30, 60 s; it connects again on the first retry after the file was fixed |
| Hub restart | "Connection closed", retries every 10 s, reconnected a second after the hub came back |
| Silent network drop (proxy stops forwarding, connections stay open) | The 120 s receive deadline closed the connection; each further attempt timed out at the upgrade after 30 s; it reconnected 1 s after the path came back |
| TLS through a reverse proxy under `/beszel`, with `CA_CERT_FILE` | Connected |
| TLS without the CA | Refused: "certificate not trusted: unable to get local issuer certificate (set CA_CERT_FILE to the CA that issued the hub's certificate)" |
| TLS with a certificate for another name | Refused: "the hub's certificate is not for <workstation-address> (IP address mismatch)" |
| A token the hub doesn't know | "hub answered 401 Unauthorized (check TOKEN)", retried every 10 s |
| The user's hub (https, Nginx Proxy Manager, Let's Encrypt) | Both nodes connected with the kit's CA bundle; the user saw both systems up |

## Found and fixed

- **The LAN byte counters stick at 4 GiB** (NOTES.md 41, D15). A device whose counter stuck
  is now read through LANCP every 30 s, unprivileged.
- **An SSLOG trace ran away** (NOTES.md 42). It was mine: it used 2.4 GiB of IA64's system
  disk for an hour before it was found and cleaned up. CLAUDE.md warns against the method.
- **The first TLS handshake cost about 7 s of CPU on x86**, and the TLS context was built
  again for every connection. It is now built once per process (NOTES.md 34).
- **The TLS failure message suggested `CA_CERT_FILE` even for a name mismatch.** The
  message now names the actual cause.
- **Another process reading the agent's log can lag a few seconds** (NOTES.md 32). This
  only matters to test scripts.

## Soak test

Both agents ran as BESZEL, reporting to the user's hub, from 22:47 on 2026-10-14 to 22:47 on
2026-10-15. `probes/soak_monitor.com` sampled every 10 minutes, and `tools/soak_report.py`
compares the first hour with the last:

| | x86 (E9.2-4, 2 CPUs) | IA64 (V8.4-2L3, 1 CPU) |
|---|---|---|
| Duration, samples | 24.0 h, 145 | 24.0 h, 145 |
| Restarts | none (one PID throughout) | none |
| CPU | 230.0 s: 9.6 s an hour, 0.16 s a minute | 173.7 s: 7.2 s an hour, 0.12 s a minute |
| Working set | 33,408 pagelets, flat | 64,784, flat |
| Process pages | 11,856 → 11,872 (+16, 0.1%, in the first hours, then flat) | 9,696, flat |
| Page-file use (virtual memory in use) | 20,464, flat | 15,024, flat |
| Peak virtual size | 250,880, flat | 198,272, flat |
| Buffered I/O | about 9,850 an hour | about 11,970 an hour |

No sign of a leak: page-file use and peak virtual size, which track allocated memory, didn't
move on either node.

What happened during the run (NOTES.md 40, 41, 43):

- **03:04, the hub's nightly backup:** both agents reconnected within a minute; the hub
  closed the connection again at 03:33, 03:59 and 04:05, and each reconnect took 0–5 s.
- **13:38, IA64:** EWB0's 32-bit "bytes received" counter stuck at 4,294,967,295; the agent
  logged it. Fixed by D15 (below) and deployed after the run.
- **About 18:33, a network glitch:** IA64 saw a silent drop that its 120 s deadline caught
  (reconnected after 20 s); x86's connection failed outright and reconnected after 4 s.

An earlier run on the build from before the security fixes (as IAIN, 21:52–22:18 on
2026-10-14) was stopped when the agents moved to their account.

## After the soak test

Deployed on both nodes at 22:48 on 2026-10-15, as BESZEL:
- **`3cb5e0a`:** connection errors name the address (NOTES.md 40).
- **`350f188`:** LANCP for a stuck LAN counter (D15). IA64 logged the switch for EWB0 at start,
  and LANCP works as the unprivileged BESZEL account.

Both agents reconnected to the user's hub.

## Security review

An independent review (`docs/SECURITY_REVIEW.md`) found 4 medium, 6 low and 8 informational
issues, and nothing high. Every one has a response in that file. The main changes:

- **M1:** all socket and TLS I/O is non-blocking with deadlines, so a stalled peer can no
  longer hang the agent.
- **M2 (D12, D14):** the agent never runs as SYSTEM or with privileges. Starting it as its
  own account turned out to need a persona launcher (`BESZEL_LAUNCH.EXE`):
  `RUN/DETACHED/UIC` keeps the creator's username and authorized privileges (NOTES.md 36).
- **M3 (D13):** a warning for `http://` hubs.
- **M4, L1, L2:** log flooding, control characters and quadratic parsing.
- **L3, L4, L5, I2–I7:** logical-name tables, the process lookup, the fingerprint file, and
  secret wiping and input checks.

## The service account (BESZEL)

Created on both nodes with the user's approval (UIC [377,1]). Its settings, all tested:
- no DisUser, because personas refuse disusered accounts;
- every login class disallowed;
- `LGICMD NL:`;
- NETMBX and TMPMBX only.

The refusal cases behave as D12 requires:
- START as SYSTEM: refused by START;
- a misspelt account: refused by START;
- an account with privileges: refused by the agent, and the run loop stops.

Run as BESZEL, the agent passes its own check and has reported to the user's hub since
22:40 (x86) and 22:45 (IA64).

## For the exit review

| # | Question | Recommendation |
|---|---|---|
| D12–D15 | No system accounts or privileges; warn about `http://`; start through a persona launcher; LANCP for stuck counters | Already your decisions (D12, D13, D15) or forced by the platform (D14). Confirm at the exit. |
| Test trees | Both nodes run the agent from test trees in my work directory, under the BESZEL account, reporting to your hub | Leave them running until the kit replaces them in Phase 4, or stop them now. Your choice. |

## Next: Phase 4

The PCSI kits for x86-64 and IA64:
- the BESZEL account, created and checked by the kit;
- the `BESZEL$ROOT` tree with the protections Phase 3 found it needs;
- a `SYS$STARTUP` entry;
- the CA bundle;
- the configuration template.

Then an install test on each node (system-wide: ask first), and the release as
`v0.21.0-vms1`.
