# Phase 3: service and robustness (report)

**Status:** in progress (2026-10-14). The service, disconnection tests, security review and
its fixes are done. The 24-hour soak test on the final build (both agents as the BESZEL
account, reporting to the user's hub) runs from 22:47 on 2026-10-14 to about 23:47 on
2026-10-15.

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

- **The first TLS handshake cost about 7 s of CPU on x86**, and the TLS context was built
  again for every connection. It is now built once per process (NOTES.md 34).
- **The TLS failure message suggested `CA_CERT_FILE` even for a name mismatch.** The
  message now names the actual cause.
- **Another process reading the agent's log can lag a few seconds** (NOTES.md 32). This
  only matters to test scripts.

## Soak test (running)

Both nodes, `probes/soak_monitor.com` every 10 minutes into `[.SOAK]SOAK.CSV`. A first run
on the build from before the security fixes (running as IAIN, 21:52–22:18) was stopped
when the agents moved to their account; its data is kept as `[.ROOT.LOG]SOAK_IAIN.CSV`.

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
