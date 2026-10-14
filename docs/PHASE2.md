# Phase 2: the OpenVMS platform layer (report)

**Status:** in progress (2026-10-14). The platform layer is done and verified on both nodes.
Two exit items are open:
1. **The LAN counter crossing 2^32** on IA64 (EWB0, about 15:00 on 2026-10-15): do the
   counters wrap, which the agent already handles, or stick at the maximum?
2. **Both nodes in the user's LAN hub.** This needs the user to add the systems, or to give
   a universal token (below).

## What was built

- **`src/plat/vms/bz_plat_vms.c`** implements `bz_platform.h` with system services. Nothing
  needs privilege; the agent runs with NETMBX and TMPMBX only. For each metric:

  | Metric | Source and rule |
  |---|---|
  | CPU, breakdown, per CPU | `RMI$_MODES`, sized from its CPU count. Each CPU's times are accumulated from interval deltas: total = max(Δ[0], Δ[7]) + the other modes. User = user + super; system = kernel + exec + MP synch + interrupt + compat. |
  | Load average | Linux damping of CUR − 1 + COM + COMO. The −1 leaves out the agent, which is always current when it asks. |
  | Memory | `MEMSIZE`; used = total − free list − modified list. Buffer/cache is 0, because XFC's size has no public interface, so XFC counts as used. |
  | Swap | Page files plus swap files; used = size − free |
  | Disks | `$GETDVI` MAXBLOCK/FREEBLOCKS on mounted volumes. The root is `SYS$SYSDEVICE:`, or `FILESYSTEM`. |
  | Network | LAN read-counters on each device's unit 0, through channels opened once. The 32-bit counters are extended to 64 bits and read at least every 5 s. A counter at its maximum logs a warning. |
  | Identity | `$GETSYI`, and the TCP/IP host name. Fingerprint seed: `SYSTEM_UUID`. |
  | Settings | Logical names `BESZEL$<NAME>` (LNM$FILE_DEV) override the file |

- **`build_vms.com`** now also links `[.BUILD]BESZEL_AGENT.EXE`.
- **`beszel_agent --sample [secs] [disk...]`** prints what the agent would report, without a
  hub. It's used for the cross-checks below, and gives users a way to check an
  installation.
- **Test tools:**
  - `probes/check_metrics.com`: the agent against MONITOR and SHOW over the same window.
  - `tools/vmsrun.sh` with `probes/run_agent.com`: run the agent on a node for a set time.
  - `tools/refhub.sh` with `REFHUB_BIND`: lets the nodes reach the reference hub.

## Found on the nodes (NOTES.md 23–31)

| # | Finding | Effect, and the fix |
|---|---|---|
| 23 | `$GETRMI` writes more than the 16-bit return length an item list provides | Overwrote the stack and crashed the sampler. Every return-length field now has 8 bytes. This also explains Phase 0's "wrong" return lengths. |
| 24 | No public interface reports the XFC size | Buffer/cache is 0 (D9 closed) |
| 25 | CRTL `usleep()` returns at once for a second or more | `sleep()` for whole seconds |
| 26 | `/STANDARD=C99` hides `SIGPIPE` and `localtime_r` | Guarded; plain `localtime` |
| 29 | x86 idle can advance more than interrupt+idle within an interval | Per-interval accumulation; breakdowns now sum to 100 |
| 30 | `RMI$_CUR` counts the agent itself | Subtracted; an idle node's load reads about 0.02 instead of about 1.0 |

## Verification

- **Against MONITOR and SHOW** (`check_metrics.com`, the agent unprivileged, 10 s windows):

  | | Agent | System |
  |---|---|---|
  | x86 busy | 39.34% (5.57 user + super, 33.77 system, 60.66 idle) | MONITOR 37.8% (5.49, 31.7), its window about 1 s apart |
  | IA64 busy | 21.88% (8.69 / 13.19) | MONITOR 21.7% (8.70 / 12.88) |
  | Memory, swap, disks, uptime | | SHOW MEMORY, SHOW DEVICE and the boot time agree on both nodes |

- **Against upstream's hub** (the reference hub, reachable from the nodes for the test):
  - both agents ran unprivileged, 5 minutes at a time;
  - every minute's record arrived, with details, extra filesystems, per-NIC rates and
    per-CPU figures;
  - the breakdowns sum to 100;
  - load averages are realistic.
- **Unit tests** are unchanged: 221/221 on Linux and on both nodes.

## Open items

1. **The LAN counter crossing.** EWB0 had received 4,123,688,698 bytes at 20:19 on
   2026-10-14, and 2^32 is 4,294,967,296. At its rate it crosses around 15:00 on
   2026-10-15.
   - If it **wraps**, the agent already extends it, and this item closes.
   - If it **sticks**, LANCP's wider interface must be found, or the limit documented with
     a warning (which the agent already logs).
2. **The user's LAN hub.** Two ways:
   - **(a) A universal token.** You create one in the hub (Settings, Tokens) and give it to
     me with the hub's public key. Each node then adds itself, which also tests that
     path.
   - **(b) Add the systems yourself.** Add the two systems in the hub's UI ("Add system")
     and give me each one's token and the key.

   Either way the token goes only into the owner-only config file on each node.

## Next

Then the exit review, and Phase 3: running as a detached service under its own account,
a stop mechanism, reconnect tests (hub restart, network drop, TLS), and a 24-hour soak test
on both nodes.
