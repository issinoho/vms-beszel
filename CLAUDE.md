# CLAUDE.md

A native OpenVMS agent for [beszel](https://github.com/henrygd/beszel), so that OpenVMS
systems (x86-64 first, IA64 second) report to an unmodified beszel hub. Read
`docs/PLAN.md` (the plan) and `docs/DECISIONS.md` (every decision, with the alternatives)
before starting work. Work phase by phase; stop and ask at each phase exit.

Upstream's agent is Go, which does not exist on VMS, so this is a **C re-implementation
of the protocol, not a patched port** (D1). The pinned upstream tree (`upstream.conf`,
fetched into `cache/`) is the specification and the test oracle. Every protocol fact
in code or docs cites the upstream file it came from.

## Ground rules (from the sibling vms-* repos)

- **Never edit `cache/` or `out/`.** Upstream is fetched at the pinned tag and commit by
  `tools/fetch.sh`, never edited.
- **Use `tools/vms.sh`** for remote work; never raw `ssh host cmd`, never `WAIT` over ssh.
  Run long jobs with `run_in_background`, and probes that might hang as batch jobs.
- **Committed files must not contain real node or hub details** (they live in the
  git-ignored `tools/nodes.conf` and `tools/hub.conf`), nor credentials, tokens or keys.
  Use `<x86-host>`, `<ia64-host>`, `<hub-host>`.
- **Nothing destructive or system-wide on the nodes** without asking: no DELETE/PURGE
  outside our work directories, no PRODUCT INSTALL, no new accounts, no system logical
  names, no startup changes. The LAN hub is the user's: add or remove test systems in it
  only when asked.
- Every decision goes in `docs/DECISIONS.md`; every platform surprise in `docs/NOTES.md`
  with how it was observed; every build failure in `docs/PORTING_LOG.md`.
- Prefer a 20-line probe in `probes/` over speculation. Do not claim something works until
  a test on the node (or against the hub) shows it.

## Hard constraints

- **C99, VSI C, 32-bit pointers** (D2). The Linux build uses `-std=c99 -pedantic -Wall
  -Wextra`; do not weaken it.
- **`src/core/` does no I/O** and has no platform dependencies. **`src/plat/bz_platform.h`
  is the entire OS surface** and contains no VMS types; `plat/vms/` and `plat/stub/`
  implement it.
- **Wire formats use explicit encoding**, never struct layout. CBOR keys and omit rules
  follow upstream's struct tags exactly; a new field needs a conformance test.
- **Never log or echo the token**, and never log signature bytes. The hub's signature
  must verify before any action but `CheckFingerprint` is answered.
- **Never change the fingerprint** once it is written to `[DATA]`: the hub rejects the
  agent if it does.
- **The hub must not change.** If something seems to need a hub change, stop and ask.

## OpenVMS

Undefined symbols are *warnings* on VMS: check `$SEVERITY` after every LINK. Build from
CLEAN after changing headers or qualifiers. C diagnostics go to SYS$ERROR: `define
sys$error sys$output` in procedures run by vms.sh. Take item codes from the headers on the
node (`LIBRARY/EXTRACT` from `SYS$STARLET_C.TLB`), never from memory. The sibling
CLAUDE.md files (vms-steward, vms-lighttpd) list more pitfalls, and they apply here.

## Commits

Commit in logical steps with messages that explain why, particularly the reasoning
behind a platform workaround, and record what was verified and how. Don't push without
the user asking; the remote will be `origin` (github.com/issinoho/vms-beszel).
