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
