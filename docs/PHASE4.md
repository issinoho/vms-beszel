# Phase 4: kit and release (report)

**Status:** done (2026-10-16). Exit review: approved by the user on 2026-10-16: the
product name `BESZEL` (D0), the boot lines left to the user, and the release as
`v0.21.0-vms1`.

**Verdict:** the PCSI kits install, configure, start, remove and reinstall cleanly on
x86-64 and IA64. Both agents now run from the kit as BESZEL and report to the user's hub. The
install test found two faults, both fixed: a logical name left behind by `BESZEL$CONTROL`, and
the unexplained "protocol wrong type for socket" from Phase 3, which hid the real connection
error.

## The kits

| Kit | SHA-256 |
|---|---|
| `ISSINOHO-X86VMS-BESZEL-V0021-0E1-1.PCSI` | `a9143320ad919548b28999d0cc7337bf4c9efa0024390c4a74d9074540ef5a7c` |
| `ISSINOHO-I64VMS-BESZEL-V0021-0E1-1.PCSI` | `8446d5753f5ecc61db41fe30070f0d23467be5a21bb14b8fdc7c6e33ce78536e` |

`tools/kit.sh <node>` builds the agent on the node and runs its native tests (228 checks, 0
failed on both). It then makes the kit with `kit/make_kit.com` and fetches it to `out/kits/`,
with `SHA256SUMS`. The versions come from `src/core/bz_version.h` alone: agent 0.21.0-vms1,
PCSI V0.21-0E1. The product is `BESZEL`, not `BESZEL_AGENT`: the longer name made the kit name
40 characters, one more than PCSI accepts (D0, amended).

| Piece | What it does |
|---|---|
| `SYS$COMMON:[BESZEL]` | `[BIN]` the agent and its launcher; `[COM]` the procedures; `[CONF]` the template and the CA bundle (curl.se, pinned in `upstream.conf`); `[DOC]` README.VMS and the licence; `[DATA]` and `[LOG]` empty |
| `SYS$STARTUP:BESZEL$STARTUP.COM` | At boot: defines `BESZEL$ROOT` and `BESZEL$USER`, and starts the agent if the site settings say so. The kit also runs it with `INSTALL` (`BESZEL$ROOT` only) and `REMOVE` (stop, deassign) |
| `SYS$STARTUP:BESZEL$SHUTDOWN.COM` | Stops the agent, which says goodbye to the hub |
| `BESZEL$ROOT:[COM]BESZEL$CONFIGURE.COM` | Creates the account, or re-checks one it created; sets the tree's owners and protections; makes `AGENT.CONF` from the template if there is none; writes `SYS$MANAGER:BESZEL$CONFIG.COM` (the account, and whether to start at boot) |

`BESZEL$CONFIGURE` refuses a system-group UIC, and any existing account whose owner field
isn't "beszel agent", so it can't change a real user's account (D12). Its AUTHORIZE work
file, which holds the new account's random password, sits in `[CONF]` and is deleted with
`/ERASE`.

## Install test (`probes/install_test.com`, approved: both nodes, start at boot YES)

| Step | x86 (E9.2-4) | IA64 (V8.4-2L3) |
|---|---|---|
| Stop the Phase 3 test-tree agent | done | done |
| `PRODUCT INSTALL BESZEL` | `BESZEL$ROOT` = `<sysdisk>:[SYS0.SYSCOMMON.BESZEL.]` | same (second attempt; see below) |
| `BESZEL$CONFIGURE BESZEL "" YES` | The existing BESZEL account was re-checked; owners and protections set | same |
| Carry over `AGENT.CONF` and the fingerprint from the test tree | done | done |
| `BESZEL$CONTROL START` | Runs as BESZEL with TMPMBX and NETMBX only; connected | same; connected at 08:26:17 |
| `PRODUCT REMOVE BESZEL` | Agent stopped; `BESZEL$ROOT` and `BESZEL$USER` deassigned | same |
| Reinstall, configure, start | Reconnected at 08:38:30 with the kept `AGENT.CONF;11` and `FINGERPRINT.;1` (no fingerprint mismatch) | Reconnected at 08:37:05, the same way |

Tree after configuring: `[BIN]`, `[COM]` and `[DOC]` belong to SYSTEM and are world-readable;
`[CONF]`, `[DATA]` and `[LOG]` belong to BESZEL, with no group or world access; `AGENT.CONF` is
(S:RWED,O:RWED,G,W). The Phase 3 test trees (`[.ROOT]`, `[.RUN]`, `[.SOAK]`) are deleted from
both nodes with `/ERASE`; the soak data is in `out/soak/`.

### OpenVMS Alpha (AXPBOX, V8.4-2L2; approved by the user, 2026-11-13)

No test tree and no hub on the Alpha, so the test installs, creates the account, starts
the agent as it, and removes; `probes/install_test.com`'s carry-over steps don't apply.

| Step | Result |
|---|---|
| Kit | `ISSINOHO-AXPVMS-BESZEL-V0021-1E1-1.PCSI` (409600 bytes) after `build_vms TEST`: 229 checks, 0 failed. Rebuilt afterwards by the batch-mode `kit.sh`, from the same source (a new SHA-256: PCSI records the build time) |
| UIC group 377 free | AUTHORIZE: `%UAF-W-BADSPC, no user matches specification` |
| `PRODUCT INSTALL BESZEL` | `BESZEL$ROOT` = `AXPBOX$DKA0:[SYS0.SYSCOMMON.BESZEL.]` |
| `BESZEL$CONFIGURE BESZEL [377,1] NO` | Account created: owner "beszel agent", no login class, `DisCtlY LockPwd`, NETMBX and TMPMBX; `[CONF]`, `[DATA]`, `[LOG]` BESZEL's with no group or world access; `[BIN]`, `[COM]`, `[DOC]` world-readable; AGENT.CONF from its template |
| `BESZEL$CONTROL START` | `beszel_launch: process 000025BA started as BESZEL`. With the template's placeholder key the agent exits (`KEY: key line 1: invalid ssh-ed25519 key data`) and BESZEL$RUN restarts it after 15, 30, 60 s, as designed |
| `BESZEL$CONTROL STOP` | During the 60 s wait: `did not stop in 20 s: stopping process` (the run loop checks STOP. only between runs). The STOP. file stays; START deletes it |
| `PRODUCT REMOVE BESZEL` | `BESZEL$ROOT` and `BESZEL$USER` deassigned; no agent process; `[CONF]AGENT.CONF`, `[DATA]STOP.` and the log kept |

The BESZEL account and SYS$MANAGER:BESZEL$CONFIG.COM remain on AXPBOX.

**Joined the user's hub (approved, 2026-11-13).** The kit reinstalled, `BESZEL$CONFIGURE
BESZEL "" YES` (the existing account re-checked, start at boot YES), and `AGENT.CONF;2` with
the hub's `HUB_URL` (as on x86), `KEY` and a token for the new system from its Add System
dialog, owned by BESZEL with (S:RWED,O:RWED,G,W); the copy used to carry it was erased.
`BESZEL$CONTROL START`: `BESZEL$RUN starting … as BESZEL with TMPMBX,NETMBX`, then
`WebSocket connected host=<hub-host>` six seconds later. SYSTARTUP_VMS.COM;43 runs
BESZEL$STARTUP and SYSHUTDWN.COM;10 BESZEL$SHUTDOWN (recorded in the site's own
configuration). Not yet seen: a real boot starting it.

## Release 0.21.1-vms2: OpenVMS Alpha (2026-11-13)

A dedicated Alpha release (the family's policy for Alpha kits). Its only change from vms1 is
`rmi_u()`'s longword retry (NOTES.md 47), which matters only on Alpha, so x86-64 and IA64 stay
on vms1 and vms2 has the AXPVMS kit alone. Built for OpenVMS Alpha V8.4-2L1 or later
(`/ARCHITECTURE` left at GENERIC); tested on V8.4-2L2.

| Step | AXPBOX (Alpha V8.4-2L2, VSI C V7.4-002) |
|---|---|
| `tools/kit.sh alpha` | `ISSINOHO-AXPVMS-BESZEL-V0021-1E2-1.PCSI`, 229 checks, 0 failed; SHA-256 `0593c916b5a96bedd9fa748409054410c247bdeb05e1d9890334688e09ff1781` |
| `probes/upgrade_test.com` (approved) | Over V0.21-1E1: `PCSI-I-FORCEDUPG … V0.21-1E1 implicitly removed`, V0.21-1E2 installed; `BESZEL$USER` deassigned and `BESZEL$STARTUP` run as a boot would: `started as BESZEL`, `with TMPMBX,NETMBX`, `beszel agent for OpenVMS 0.21.1-vms2`, `WebSocket connected` to the user's hub ten seconds later, with the kept AGENT.CONF and fingerprint |

## Found and fixed

- **`BESZEL$CONTROL` left a process logical name behind** (NOTES.md 44). On IA64, stopping the
  test-tree agent left a process `BESZEL$ROOT` pointing at the test tree. That name hid the
  kit's system one: `BESZEL$CONFIGURE` "wasn't found" (RMS-E-FNF), and the carry-over copied
  the test tree onto itself. `BESZEL$CONTROL` now names its files from its own location and
  defines no logical names. The kit was rebuilt, the half-done install removed, and the test
  run again from the start.
- **"protocol wrong type for socket" explained** (NOTES.md 45, `probes/probe_gai.c`). For a
  name that DNS answers, VMS's `getaddrinfo()` returns every address twice, as TCP and as SCTP.
  `socket()` refuses the SCTP copy. When the hub was unreachable, the last error kept was the
  SCTP one, which hid the real one. The agent now asks for `IPPROTO_TCP`.
- **`tools/kit.sh`**: two runs at once shared `out/kitstage`, and a failed fetch left the old
  kit in `SHA256SUMS`. It now takes a lock and checks the fetch.

## For the exit review

| # | Question | Recommendation |
|---|---|---|
| D0 | The PCSI product is `BESZEL` (the 39-character limit) | Confirm |
| Boot | The lines in `SYSTARTUP_VMS.COM` and `SYSHUTDWN.COM` are yours to add on each node | Add them when convenient. Until then the agent runs, but won't come back after a reboot |
| Release | Tag `v0.21.0-vms1`; a GitHub release with both kits, `SHA256SUMS` and notes in the family format; add vms-beszel to the family site (`vms-grep/site/projects.json`) and vms-grep's README | Go ahead when you say so |

## vms2 (2026-10-18): the boot path

After both nodes rebooted on 2026-10-17, neither agent came back: vms1's `BESZEL$STARTUP`
never started the agent at boot (NOTES.md 46). The install test above had started it with
`BESZEL$CONTROL START`, so it never ran that path. vms2 fixes the startup procedure and
nothing else.

| Kit | SHA-256 |
|---|---|
| `ISSINOHO-X86VMS-BESZEL-V0021-0E2-1.PCSI` | `a2223014cf1ac3c6fa8e5bf12f349243fc92416ae331827e16a70291953328ae` |
| `ISSINOHO-I64VMS-BESZEL-V0021-0E2-1.PCSI` | `edec1b8d43af62b135cf17702db6a231fec9403edb8a3cb97e1ece6cd62605fa` |

Upgrade test (`probes/upgrade_test.com`, approved: both nodes). Native tests: 228 checks,
0 failed, on both. On each node: `BESZEL$CONTROL STOP`, then `PRODUCT INSTALL` of vms2 over
vms1 ("V0.21-0E2 installed, V0.21-0E1 removed"), then `BESZEL$USER` deassigned and
`@SYS$STARTUP:BESZEL$STARTUP` run as a boot would. The startup defined `BESZEL$USER = BESZEL`
and started the agent as BESZEL with TMPMBX and NETMBX only. It logged 0.21.0-vms2 and
connected to the hub with the kept `AGENT.CONF` and fingerprint (x86 01:16:12, IA64 01:18:17).

## 0.21.1-vms1 (2026-10-23): following upstream 0.21.1

Upstream released beszel 0.21.1 on 2026-10-22. This release moves the pin to it and sends
memory and swap in bytes (DECISIONS.md D16, PROTOCOL.md section 10). Linux tests: unit and
sanitizer builds, fuzzing, the conformance tests against the 0.21.1 tree, and
`tests/integration/itest.py` against the 0.21.1 release hub.

| Kit | SHA-256 |
|---|---|
| `ISSINOHO-X86VMS-BESZEL-V0021-1E1-1.PCSI` | `1763cd128bdf0c585f0ec3a59059e7ec2df4a72c3869de981a7c44d0446ba12c` |
| `ISSINOHO-I64VMS-BESZEL-V0021-1E1-1.PCSI` | `f4a24adcbf50edb5c9fc58a25aedddcb69c3491b78fadca6a01d2a479287277f` |

Native tests: 229 checks, 0 failed, on both. The first IA64 `tools/kit.sh` run came back with
an empty build log after the `@build_vms TEST` step; a rerun built and passed, so it was the
remote session, not the build.

Upgrade test (`probes/upgrade_test.com`, approved by the user and run by them from this
session, both nodes): `BESZEL$CONTROL STOP`, then `PRODUCT INSTALL` of vms1 over 0.21.0-vms2
("V0.21-1E1 installed, V0.21-0E2 removed"), then `BESZEL$USER` deassigned and
`@SYS$STARTUP:BESZEL$STARTUP` run as a boot would. On each node the startup defined
`BESZEL$USER = BESZEL` and started the agent as BESZEL with TMPMBX and NETMBX only. It logged
`0.21.1-vms1 (protocol 0.21.1)` and connected to the user's hub with the kept `AGENT.CONF` and
fingerprint (x86 20:28:48, IA64 20:30:26). That hub is still 0.21.0, so this also covers an
older hub, which reads the GB fields the agent still sends.

