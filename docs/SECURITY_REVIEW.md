# vms-beszel security review

Date: 2026-10-14. Reviewer: an independent read-only review of the source tree as it stood at the
end of Phase 2. Scope: `src/core/*.c`, `src/plat/bz_net_ossl.c`, `src/plat/bz_crypto_ossl.c`,
`src/plat/vms/bz_plat_vms.c`, `src/plat/stub/bz_plat_stub.c`, `src/main/bz_main.c`,
`vms/beszel$run.com`, `vms/beszel$startup.com`, `tests/unit/fuzz_core.c`, plus the
`tools/vmstree.sh` layout and `vms/agent.conf_template`, which show how the tree is protected.

Threat model: (1) a malicious network, hub or proxy sending arbitrary bytes at any stage; (2) a
rogue hub without the private key trying to get data; (3) local VMS users reading secrets or
interfering with the agent's files; (4) TLS downgrade or MITM.

Method: I read the code line by line and checked every finding against it. I ran `make test
test-asan fuzz` on Linux: 221 checks passed with and without ASan/UBSan, and 300,000 fuzz
iterations ran with no faults. I timed one finding (L2) with a scratch program that linked the
core parser. I did not run anything on the VMS nodes or touch the network. Anything I could not
confirm from the code is marked **uncertain**.

Summary: the parsers (CBOR, WebSocket, HTTP upgrade, config, key, URL) are careful, and I found
no memory-safety bug in them. The gate "nothing but CheckFingerprint before the signature
verifies" is implemented correctly. TLS verification is always on and fails closed. The real
risks are operational. The TLS socket blocks with no read timeout. By default the procedure runs
the agent as whoever starts it, which at boot is SYSTEM. `http://` hubs are accepted without a
word, and with them the signature check protects nothing. A peer that has not proved its key can
also make the agent write a log line for every tiny frame it sends.

---

## Medium

### M1. TLS I/O blocks with no deadline: one stalled record hangs the agent indefinitely

- **Where:** `src/plat/bz_net_ossl.c:92-96` (the socket goes back to blocking mode),
  `:168-171` (handshake under `SO_RCVTIMEO`), `:189-191` (`SO_RCVTIMEO` set to 0, which means
  "wait forever"), `:256` (`SSL_read`); `src/main/bz_main.c:399-402`.
- **What happens:** after the handshake the socket is blocking with no receive timeout. The main
  loop calls `select()`, then `SSL_read()`. On a blocking socket, `SSL_read` does not return
  `WANT_READ` on a partial record. It waits until the whole record arrives. The `-2` "partial
  record" branch in `bz_net_recv` (`:262-264`) and in `bz_main.c:403` is therefore never taken
  on this path.
- **Scenario:**
  - The hub or proxy sends a TLS record header and part of its body, then stops. An on-path
    attacker with no certificate can do the same: forward the first TCP segment of a real
    record and drop the rest. A NAT or route failure in the middle of a record has the same
    effect, without anyone attacking.
  - The agent then sits in `SSL_read` for ever.
  - The 120 s receive deadline (`bz_agent_tick`), the reconnect logic, the 2 s check for the
    stop file and the SIGTERM flag all run in the main loop, so none of them runs.
  - The hub shows the system down until someone stops the process. `BESZEL$STARTUP STOP` falls
    back to `STOP/ID` after 20 s.
  - The handshake has a related problem. `SO_RCVTIMEO` (15 s) applies to each `recv()`, not to
    the whole handshake. A peer that sends one byte every 14 s keeps `SSL_connect` going for
    hours, and it needs no certificate to do so, because the certificate is checked only once
    the Certificate message is complete.
- **Contrast:** without this bug, an attacker who drops traffic causes only a reconnect after
  120 s, and the agent recovers by itself when the network does.
- **Uncertain:** I could not check whether TCP/IP Services honours `SO_RCVTIMEO`/`SO_SNDTIMEO`
  on VMS. If it doesn't, the handshake and `SSL_write` can block for ever as well.
- **Fix:**
  - Keep the socket non-blocking after connect. Drive `SSL_connect`, `SSL_read` and `SSL_write`
    with `select()`, treat `SSL_ERROR_WANT_READ`/`WANT_WRITE` as "wait", and give each
    operation an overall deadline (for example 15 s for the handshake).
  - A cheaper partial fix is a non-zero `SO_RCVTIMEO` (say 5 s) after the handshake, with a
    timeout treated as `-2`. That bounds the stall, but it does not stop a slow drip during the
    handshake.
  - Consider `SO_KEEPALIVE` as well.

### M2. By default the agent runs as the caller (SYSTEM at boot), and only its current privileges are dropped

- **Where:** `vms/beszel$startup.com:51-52` and `:94-101`; `vms/beszel$run.com:12`.
- **What happens:**
  - If the logical name `BESZEL$USER` is not defined, `RUN/DETACHED SYS$SYSTEM:LOGINOUT` gets
    no `/UIC`. The process then runs under the caller's username and UAF record. From
    SYSTARTUP that is SYSTEM: UIC [1,4], system-category file access and all authorized
    privileges.
  - `SET PROCESS/PRIVILEGES=(NOALL,NETMBX,TMPMBX)` in `BESZEL$RUN.COM` clears only the
    *current* privileges. The authorized set is unchanged. Any code running in the process can
    turn them back on with `$SETPRV`, and the UIC still gives SYSTEM file access.
- **Scenario:**
  - Any future memory-corruption bug reachable from the hub or the network (the parsers are
    exactly that surface) becomes full system compromise, not compromise of an unprivileged
    account.
  - With UIC [1,4], the agent can also read and write every file whose protection grants S:
    access.
- **Likely, not tested on a node:** if `BESZEL$USER` is misspelt, `F$IDENTIFIER` fails. Under
  `SET NOON` the assignment at `:96-97` is abandoned, `uic` stays `""`, and the agent quietly
  runs as the caller (SYSTEM) instead of failing.
- **Fix:**
  - Make a dedicated account mandatory. `BESZEL$STARTUP` should refuse to start when
    `BESZEL$USER` is undefined or does not resolve. Check the result of `F$IDENTIFIER`
    explicitly instead of relying on `SET NOON`.
  - Create the account (in the kit) with only NETMBX and TMPMBX as *authorized* privileges, in
    a UIC group of its own that is above MAXSYSGROUP.
  - In `BESZEL$RUN.COM`, refuse to run if `F$GETJPI("","AUTHPRIV")` holds more than
    NETMBX and TMPMBX, or if the UIC group is ≤ MAXSYSGROUP.

### M3. `http://` hubs are accepted silently, and over them the hub signature protects nothing

- **Where:** `src/core/bz_url.c:56` (http is accepted); `src/core/bz_ws.c:36` and
  `src/core/bz_agent.c:81` (the token goes out in `X-Token` before the hub has proved
  anything); `src/core/bz_agent.c:179-189` (the check is over the token alone).
- **What happens:** the hub signs only the token, and it signs whatever token arrives in an
  upgrade (PROTOCOL.md 2.2). There is no nonce from the agent.
- **Scenario (http, an on-path attacker or anyone who spoofs DNS for the hub):**
  1. The agent connects and sends `X-Token`. The attacker now has the token.
  2. The attacker opens its own upgrade to the real hub with that token. The hub answers with
     `CheckFingerprint` and a valid signature over the token.
  3. The attacker relays that signature to our agent. It verifies, and the agent sends its
     fingerprint and then answers every `GetData`.
  4. The attacker relays the fingerprint to the hub, so the hub binds the attacker's session.
  - Result: the attacker reads the statistics, and can feed the hub false statistics or
    suppress alerts.
  - A passive observer gets the token, the fingerprint and the (static) signature, which is
    enough to do the same later.
- **Why D11 doesn't cover this:** D11 rightly refuses `wss://`, but plain `http://` is a
  documented, supported choice, and nothing tells the operator that it removes protection
  against threats (2) and (4). Upstream behaves the same way, but this agent can be stricter
  without any change to the hub.
- **Fix:**
  - Log a WARN at every start when `HUB_URL` is `http://` and the host isn't a loopback
    address. The warning should say the token and data travel in clear and the hub check can be
    relayed.
  - Better: require an explicit opt-in setting (an extension of ours, documented as such) for
    non-loopback `http://`.
  - Say this in `agent.conf_template` next to `HUB_URL`.

### M4. A peer that has not proved its key can make the agent write log lines without limit (disk exhaustion)

- **Where:** `src/core/bz_agent.c:198` (ERROR "Error parsing message"), `:205` (WARN "hub not
  verified"), `:176` and `:184` (failed fingerprint requests); the log file opened at
  `vms/beszel$startup.com:100-101`. It is purged only at START (`:92-93`).
- **What happens:** every message that fails to parse, and every action sent before
  verification, writes one line at the default level (INFO), with no rate limit. The smallest
  frame that does this is about 5 bytes (`82 03 a1 00 00`), and each line is about 70 bytes. So
  each 16 KB read produces about 3,000 lines, roughly 230 KB of log: about 14× amplification.
- **Scenario:**
  - A malicious hub or TLS proxy, or any MITM on an `http://` hub (M3), streams minimal frames.
  - At 1 MB/s that is about 14 MB/s of log into one version of `BESZEL_AGENT.LOG` on the device
    holding `BESZEL$ROOT`, which is often the system disk.
  - The file keeps growing until the agent restarts. Every bad `CheckFingerprint` also costs up
    to 8 Ed25519 verifications.
- **Fix:**
  - Rate-limit these messages, for example by logging the first N per connection and then a
    count.
  - Or close the connection after a few protocol errors, or after any action other than
    `CheckFingerprint` arrives before verification. The 0.21.0 hub never sends one, so closing
    costs nothing.
  - Consider a deadline for verification as well (see I6).

---

## Low

### L1. Text from the hub reaches the log unfiltered (log forging)

- **Where:** `src/core/bz_agent.c:270-272` (close reason, up to 200 bytes); `src/core/bz_ws.c:123`
  (HTTP status text); `src/plat/vms/bz_plat_vms.c:211` (printed as is).
- **Scenario:**
  - A peer that is not verified sends a close frame whose reason contains `\r\n2026-10-14
    12:00:00 INFO  WebSocket connected host=...`. The log then shows a forged line.
  - ESC sequences are passed through to whoever TYPEs the log on a terminal.
  - The status text stops at CR or LF, but other control characters get through.
- **Fix:** replace bytes below 0x20, and 0x7F and above, with `?` or `\xNN` before logging any
  text the peer controls.

### L2. The frame parser costs time quadratic in the number of frames per read (CPU amplification)

- **Where:** `src/core/bz_buf.c:101-106` (`bz_buf_consume` memmoves the rest of the buffer),
  called once per frame from `src/core/bz_ws.c:274`, `:303` and `:316`.
- **Scenario:**
  - A 16 KB read holding 8,192 empty ping frames (`89 00`) makes 8,192 memmoves of up to 16 KB,
    plus 8,192 `RAND_bytes` calls for the pong masks.
  - Measured on the Linux workstation with the core parser alone: 3.2 MB of such frames took
    0.37 s of CPU. So about 9 MB/s keeps one fast core busy, and the VMS nodes, especially the
    rx2660, need much less.
  - No verification is needed, because pings are answered before the hub has proved its key.
- **Fix:** parse from an offset in `in` and compact once per `bz_agent_on_data` call, not once
  per frame.

### L3. Configuration overrides and `BESZEL$ROOT` are looked up in the group and job tables too, in any access mode

- **Where:** `src/plat/vms/bz_plat_vms.c:177` and `:195` (`LNM$FILE_DEV`, access mode 0 = user);
  `vms/beszel$run.com:13` and `:23`; `vms/beszel$startup.com:63` and `:95`.
- **What happens:**
  - D5 and the template say an override comes from the process table, then the system table.
  - `LNM$FILE_DEV` actually searches process, job, **group** and system, and user-mode names
    count.
  - RMS uses the same search when it opens `BESZEL$ROOT:` files, and so does `F$TRNLNM` in
    both procedures.
- **Scenario:** a user who holds GRPNAM in the agent's UIC group defines `BESZEL$HUB_URL` in
  the group table, pointing at their own server.
  - The agent sends them the token in `X-Token` at the next connection (see M3).
  - A group-table `BESZEL$ROOT` (when there is no system logical) would make `BESZEL$RUN`
    start their image.
  - GRPNAM is a group-level privilege, so the exposure is small. It matters more when the agent
    shares a group with other accounts, or runs in group 1 (M2).
- **Fix:**
  - Translate overrides with `LNM$SYSTEM_TABLE` (and `LNM$PROCESS_TABLE` if wanted) explicitly.
    Or require executive mode (`acmode = PSL$C_EXEC`) and have the kit define the names
    `/EXECUTIVE_MODE`.
  - Define `BESZEL$ROOT` `/SYSTEM/EXECUTIVE_MODE` in the kit's startup.
  - Correct the documentation to match whichever behaviour is kept.

### L4. `find_pid` matches only the process name, so another user can pose as the agent

- **Where:** `vms/beszel$startup.com:151-160`.
- **What happens:** process names are unique only within a UIC group. A user in another group
  can create a process named `BESZEL_AGENT`.
- **Scenario:**
  - `START` then says "already running" and doesn't start the agent. This matters for a manual
    restart; at boot no user process exists yet.
  - `STATUS` reports the user's process.
  - `STOP` writes the stop file, waits 20 s, then `STOP/ID`s whichever process it found first,
    which may be the user's.
- **Fix:** also compare `F$GETJPI(found,"USERNAME")` (or the UIC) with the agent's account,
  and keep looking if they differ.

### L5. Files in `DATA_DIR` are trusted without checking who could have written them

- **Where:**
  - `src/main/bz_main.c:331-334` and `:363`: any file named `stop` in `DATA_DIR` stops the
    agent, whoever owns it.
  - `src/main/bz_main.c:85-93`: a fingerprint file that is empty or 49 characters or longer is
    silently replaced by a new one, which breaks CLAUDE.md's "never change the fingerprint" rule.
  - `tools/vmstree.sh:17`: only `[CONF]` and `[DATA]` are restricted; `[BIN]`, `[COM]` and
    `[LOG]` keep inherited protection.
- **Scenario:**
  - If `DATA_DIR` is set to a shared directory, or `[DATA]` is left group- or world-writable
    (the kit doesn't exist yet), a local user can stop the agent at will, or plant a
    fingerprint so the hub refuses the agent ("fingerprint mismatch").
  - Write access to `[BIN]` or `[COM]` means code execution as the agent's account, which is
    SYSTEM by default (M2).
- **Fix:**
  - At start, warn (as for `AGENT.CONF`) if `DATA_DIR` is writable by group or world, and
    ignore a stop file not owned by the agent's UIC or SYSTEM.
  - Never overwrite an existing, unusable fingerprint file. Log an error and refuse to start
    instead, so the operator decides.
  - The kit should set `[BIN]`, `[COM]` and `[CONF]` so that only SYSTEM and the owner can
    write them, and `[DATA]` and `[LOG]` to `(S:RWE,O:RWE,G,W)`.

### L6. The check that `AGENT.CONF` is private is incomplete

- **Where:** `src/main/bz_main.c:212`; `src/plat/vms/bz_plat_vms.c:129-133`.
- **What it misses:** it checks only the mode bits of the newest version of the configuration
  file. It does not look at:
  - ACLs;
  - the `[CONF]` directory;
  - older versions;
  - `TOKEN_FILE`.
- **Scenario:**
  - The template's own steps are `COPY` template, then `SET FILE/PROTECTION`, and editing comes
    after. An operator who edits *before* setting the protection, or who puts the token in with
    `COPY` from elsewhere, leaves `AGENT.CONF;1` (and so on) with the default protection,
    usually `G:RE`, and the token in it.
  - `PURGE` does not erase the blocks unless the volume has erase-on-delete.
  - A `TOKEN_FILE` that is readable by the world causes no warning at all.
- **Fix:** run the same check on `TOKEN_FILE`; in the template, say `PURGE` and `SET
  FILE/PROTECTION` on `AGENT.CONF;*`; and keep `[CONF]` closed to group and world, as
  `vmstree.sh` already does.

---

## Info

### I1. The hub's signature never changes, so anyone who has seen it once can replay it

This is a property of the protocol. The hub signs the token and nothing else, and Ed25519
signatures are deterministic, so the `CheckFingerprint` signature is a fixed value. Anyone who
has seen it can pass the agent's check as a "hub" until the hub key changes. That includes:

- a TLS-terminating reverse proxy, such as the user's Nginx Proxy Manager;
- a capture made with `tools/wscap.py`;
- an earlier `http://` deployment.

This can't be fixed without changing the hub. Document it, and treat captures (`out/`) and
proxy logs as secrets.

### I2. The token is wiped from memory only in some places

- **Where:**
  - `src/main/bz_main.c:452`: the raw `AGENT.CONF` text, with `TOKEN=`, is kept for the whole
    run and freed without zeroing.
  - `src/main/bz_main.c:250`: the `TOKEN_FILE` buffer is freed without zeroing.
  - `src/core/bz_agent.c:81`: the upgrade request in `agent.out` is cleared but never zeroed.
  - Copies left behind by `realloc` growth are never zeroed either.
- **Impact:** on VMS, another process can't read this memory without privilege, so the wiping
  that is done buys little. A process dump (an image run with `/DUMP`, or `SET PROCESS/DUMP`)
  would contain the token.
- **Fix:** either wipe consistently (zero `file` right after parsing, zero buffers before
  freeing them) or drop the partial wiping and record the reasoning in DECISIONS.md.

### I3. HUB_URL is logged in full when it doesn't parse, and credentials in it are dropped without a word

- **Where:** `src/main/bz_main.c:234`; `src/core/bz_url.c:70-72`.
- **What happens:** a `HUB_URL` with `user:password@` is written to the log in full when it
  fails to parse. When it parses, the user and password are silently dropped.
- **Fix:** refuse URLs that contain a user or password, and never echo the URL itself.

### I4. Token and URL are not checked for control characters

- **Where:** `src/core/bz_ws.c:28-36`.
- **What happens:** a CR or LF in `TOKEN` or `HUB_URL` (from the file, a logical name or
  `TOKEN_FILE`) would insert headers into the upgrade request. Only whoever controls the
  configuration can do this. The hub also rejects tokens longer than 64 bytes, and we don't
  check that.
- **Fix:** at start, reject control characters and spaces in both, and tokens longer than 64
  bytes.

### I5. An empty `CA_CERT_FILE` crashes the agent

- **Where:** `src/main/bz_main.c:281`.
- **What happens:** for an empty file, `bz_plat_read_file` succeeds and leaves `b.p == NULL`.
  `strstr(NULL, …)` is then undefined behaviour, a crash in practice. Only the configuration
  can trigger it.
- **Fix:** test `b.len` first.

### I6. A peer that has not proved its key can keep the connection open indefinitely

- **Where:** `src/core/bz_agent.c:256-267`.
- **What happens:** `last_rx` is refreshed by pings, pongs and text frames before
  verification, so a peer that never sends a valid signature can hold the connection for as
  long as it likes. Upstream behaves the same way.
- **Fix:** close the connection if verification hasn't happened within, say, 30 s of the
  upgrade.

### I7. Setting the system clock back stops the agent's timers

- **Where:** `src/plat/vms/bz_plat_vms.c:69-78`.
- **What happens:** `bz_plat_now_ms` stops at the last value it returned until the wall clock
  catches up. If an operator sets the clock back an hour, reconnects and the receive deadline
  stop for that hour. This is availability, not security.
- **Fix:** use a monotonic source. Elapsed time from `SYS$GETTIM_PREC` won't do, because it
  is wall time too. EXE$GL_ABSTIM_TICS through `$GETSYI` might; **uncertain** whether it is
  available.

### I8. The Linux stub writes the fingerprint in a way open to symlink attacks (test platform only)

- **Where:** `src/plat/stub/bz_plat_stub.c:67-86`.
- **What happens:** `fopen` follows symlinks and creates the file with the umask's permissions
  before `chmod`. That matters only if `DATA_DIR` is a shared directory on a Linux test machine.

---

## Checked and found sound

- **CBOR decoder** (`bz_cbor.c`):
  - Every length is compared with the bytes remaining as a 64-bit value before it is cast to
    `size_t` (`:222`, `:243`, `:304`, `rcontainer`). This is safe with VMS's 32-bit `size_t`.
  - Nesting is limited to 32 levels (`:197`), and so are chains of tags.
  - Indefinite-length items are bounded by the input.
  - A lone break byte or a reserved additional-info value is rejected.
  - `bz_request_parse` rejects trailing bytes.
  - IDs are limited to 32 bits and `CacheTimeMs` to 16 bits.
- **WebSocket parser** (`bz_ws.c`):
  - It rejects RSV bits, masked frames from the hub, reserved opcodes, and control frames that
    are fragmented or longer than 125 bytes.
  - The 64-bit length is checked against a 1 MB limit before any cast, and fragments together
    are held to the same limit (`:267`, `:313`).
  - Continuation and interleaving rules are enforced.
  - Memory per connection is bounded: about 1 MB each for `in`, `frag` and `msg`, and 16 KB of
    pongs per read, because `out` is flushed after every read.
- **HTTP upgrade:**
  - The response head is limited to 16 KB, and the upgrade must finish within 30 s.
  - The status must be exactly 101.
  - `Upgrade` and `Connection` are checked, and any extension or subprotocol is refused.
  - `Sec-WebSocket-Accept` is checked against a fresh 16-byte key from `RAND_bytes`.
  - Header scanning stays inside the head.
- **`bz_buf`:** `bz_buf_reserve` checks for overflow, including 32-bit `size_t`. The NUL after
  `len` is maintained, and `bz_buf_printf`'s second pass reserves `n+1`.
- **Verification gate:**
  - Before verification, only `CheckFingerprint` is acted on. Everything else is logged and
    dropped (`bz_agent.c:203-206`).
  - `verified` is reset on every connect and every close.
  - A failed check sends nothing.
  - Fingerprint, host name, port and system name are sent only after a valid signature.
  - The signature must be exactly 64 bytes, and every configured key is tried with OpenSSL's
    Ed25519, which rejects non-canonical S.
  - An empty token is refused at start (`bz_main.c:252`).
  - The signature is a public value and the result is a single bit, so timing is not a
    concern.
- **Key parsing** (`bz_key.c`): it needs the exact 51-byte SSH wire format with the name and a
  32-byte length; other key types fail with a message, at most 8 keys are accepted, and any bad
  line is fatal (as upstream). Strict base64.
- **TLS** (`bz_net_ossl.c`):
  - `SSL_VERIFY_PEER` is always set, with no option to turn it off.
  - The host name is checked with `SSL_set1_host` and SNI is sent; IP literals are checked with
    `X509_VERIFY_PARAM_set1_ip_asc`.
  - The minimum is TLS 1.2.
  - If the CA bundle or `CA_CERT_FILE` fails to load, the connection fails rather than going
    ahead unverified. If there is no bundle at all, OpenSSL's default store is used, and on SSL3
    that holds no CAs, so it also fails closed.
  - Any `SSL_connect` failure aborts the connection.
  - The cached context is rebuilt if the CA file changes.
  - `wss://` and other schemes are refused (D11).
- **The hub can make the agent do nothing but report:** every other action gets a fixed reply
  or error. No path reads files, runs commands or changes configuration. The 8-slot interval
  table bounds the memory that varying `CacheTimeMs` values can take.
- **Secrets in logs:** I found no log call that prints the token, the signature or key bytes.
  Messages are built from fixed format strings, and there is no format-string bug.
- **The VMS platform layer:**
  - The CPU count from `RMI$_MODES` is capped at 64, and the 2,628-byte buffer holds 64 CPUs.
  - Return lengths get 8 bytes (NOTES.md 23).
  - Fixed buffers from `$GETSYI` are NUL-terminated at the returned length.
  - LAN counter parsing checks each item against `iosb.cnt`.
  - The NIC name copy is bounded.
- **`BESZEL$RUN.COM` and `BESZEL$STARTUP.COM`:**
  - No symbol takes data that a user controls, other than `BESZEL$USER`, which goes through
    `F$IDENTIFIER` and `!%U`.
  - The paths come from the procedure's own location.
  - The restart back-off is bounded.
- **Tests:** `make test` and `make test-asan` passed (221/221), and `make fuzz` ran 300,000
  iterations with no ASan or UBSan fault.


---

## Response (2026-10-14)

Every finding was checked against the code before it was acted on. The fixes are tested by
unit tests (228 checks), fuzzing (6.3 million rounds), the conformance and integration tests
on Linux, and on both nodes.

| # | Response |
|---|---|
| M1 | **Fixed.** The socket stays non-blocking for its whole life. Every read and write waits with `select()` against a deadline: the TLS handshake gets 15 s in all, a send gets 30 s, and a read never blocks (a partial TLS record returns "nothing yet"). So the agent's 120 s deadline, stop file and signals keep working. |
| M2 | **Fixed** (D12, D14, the user's choice). START refuses system-group UICs and unknown accounts. The agent refuses to run with authorized privileges beyond NETMBX and TMPMBX, or with a system UIC, and the run procedure then stops instead of retrying. Starting it as the account uses a persona launcher, because `RUN/DETACHED/UIC` turned out to keep the creator's privileges (NOTES.md 36–39). |
| M3 | **Fixed** (D13, the user's choice). A WARN at every start when `HUB_URL` is `http://` and not loopback, and the README says to use https. |
| M4 | **Fixed.** A connection is closed after 10 messages that can't be used, and identical consecutive log lines are counted, with a summary every 10 minutes. |
| L1 | **Fixed.** Control characters become `?` in the log writer, on both platforms. |
| L2 | **Fixed.** The frame parser advances an offset and compacts the buffer once per read. |
| L3 | **Fixed.** Settings come from the process and system logical-name tables only. The procedures read `BESZEL$ROOT` and `BESZEL$USER` from the system table, then pin `BESZEL$ROOT` in the process table. |
| L4 | **Fixed.** The process lookup matches the account as well as the name. |
| L5 | **Partly fixed.** A fingerprint file that exists but is unreadable or damaged now stops the agent with instructions, instead of being replaced. The stop file and the directories are protected by ownership: `[DATA]` belongs to the agent's account, and only its owner and SYSTEM can create files there. |
| L6 | **Documented.** The README says to set the protection again after `EDIT` and to purge old versions. The kit will create `AGENT.CONF` with the right owner and protection. |
| I1 | **Not fixable in the agent**: it needs a challenge from the hub. Use TLS. |
| I2 | **Fixed.** The raw config text and the `TOKEN_FILE` buffer are wiped before they are freed. |
| I3 | **Fixed.** `HUB_URL` is never echoed in an error message. |
| I4 | **Fixed.** Control characters in `TOKEN` or `HUB_URL` are refused at start, and the token is limited to 255 characters. |
| I5 | **Fixed.** An empty `CA_CERT_FILE` is reported as having no certificates. |
| I6 | **Fixed.** A peer that hasn't proved its key within 60 s is disconnected. |
| I7 | **Fixed.** A clock set back is absorbed into an offset, so the timers keep running. |
| I8 | **No change**: the Linux stub is a test platform. |
