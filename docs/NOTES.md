# Notes

Platform surprises, each with how it was observed. Probe outputs are in git-ignored `out/`
(`probe_*-<node>.txt`). Nodes: x86 = OpenVMS E9.2-4 on QEMU (2 CPUs, VSI C 7.7-003); IA64 =
OpenVMS V8.4-2L3 on an rx2660 (1 active CPU, VSI C 7.4-001).

## $GETRMI

1. **Each item must be given its exact size, or the call fails with SS$_BADPARAM (20).** A
   larger buffer is not accepted. (`probe_rmi.c`; the same on both nodes, with and without
   privilege.)
   - 8 bytes: `CPUINTSTK`, `CPUMPSYNCH`, `CPUKERNEL`, `CPUEXEC`, `CPUSUPER`, `CPUUSER`,
     `CPUCOMPAT`, `CPUIDLE`, `FRLIST`, `MODLIST`.
   - 4 bytes: `COM`, `COMO`, `PROCS`, `DIRIO`, `BUFIO`, `CACHE_MEMORY`, `CACHE_FREE`.
   - Rejected at both sizes: `KERNEL`, `IDLE` (the old "primary CPU" items), `OPCNT`,
     `IOQUELEN`, `DISKS` (the DISK class needs more than an item list; we don't need it).
   - One item list holding all of them at length 4 fails as a whole. Build the list with the
     right size for each item.
2. **None of these items needs privilege.** All were read with only NETMBX and TMPMBX.
3. **`RMI$_MODES` layout** (decoded from two samples 5 s apart, then checked against the
   system-wide items):
   - a 4-byte CPU count;
   - then, for each CPU, 1 byte of CPU id and eight unsigned longwords in 10 ms ticks:
     `[0]` interrupt **including idle**, `[1]` MP synch, `[2]` kernel, `[3]` exec, `[4]` super,
     `[5]` user, `[6]` compat, `[7]` idle.

   The per-CPU sums equal `CPUKERNEL`, `CPUEXEC`, `CPUSUPER`, `CPUUSER` and `CPUMPSYNCH` to
   within ±2 ticks. Over 5 s, `[0]+[1]+…+[6]` advanced 490–499 ticks per CPU. So **total
   time = `[0]..[6]`, idle = `[7]`, interrupt = `[0] − [7]`**.
4. **`RMI$_CPUINTSTK` is negative on x86** (2^64 − 155955). It is the sum of `[0] − [7]`
   over the CPUs, and on x86 the idle counter runs a little ahead of `[0]` on a mostly idle
   CPU. On IA64 it is positive (74335 = `[0] − [7]` exactly). Clamp each CPU's interrupt
   delta at 0.
5. **`RMI$_MODES`'s return length can't be trusted.** It is 66 on x86 (the data without the
   4-byte count: 2 × 33) and 4 on IA64, although the data is there. Compute the size from
   the count: 4 + n × 33.
6. **`RMI$_CACHE_MEMORY` and `CACHE_FREE` return 0 even with all privileges**, while SHOW
   MEMORY/CACHE reports 173 MB allocated on x86. The XFC size has to come from somewhere
   else (Phase 2), or be omitted (D9).

## $GETSYI, $GETDVI

7. **No privilege needed** for any `$GETSYI` item we use, or for `$GETDVI` on disks
   (`probe_stats.c`).
8. **`SYI$_SYSTEM_UUID`** returns 16 bytes on both nodes, matching `F$GETSYI("SYSTEM_UUID")`
   with the bytes reversed. This is the VMS counterpart of the host UUID that upstream's
   fingerprint uses. `SYI$_SERIAL_NUMBER` is empty on the QEMU guest.
9. **Disks give operation counts, never bytes.** `DVI$_OPCNT` (cumulative operations) and
   `DVI$_QLEN` are the only I/O counters in `$GETDVI`. The RMI DISK class has operation count
   and queue length too, and nothing else. There is no public per-disk byte counter.
10. Free memory: `RMI$_FRLIST` (792014 pages) agrees with SHOW MEMORY "Free" (792495) taken a
    moment apart. `SYI$_MEMSIZE` (930264 × 8 KB) is SHOW MEMORY's 7.09 GB.
11. **"Swap" on VMS:** the page files and swap files are both visible through
    `SYI$_PAGEFILE_PAGE`/`_FREE` and `SYI$_SWAPFILE_PAGE`/`_FREE`. On both nodes free equals
    size, and SHOW MEMORY/FILES also shows each file's Free equal to its Size, while
    reporting 40489 pages of "total committed paging file usage" on x86. Which of the two
    beszel's "swap used" should follow is a D9 question.

## LAN counters

12. **`$QIO IO$_SENSEMODE|IO$M_CTRL|IO$M_RD_COUNT` on unit 0 of a LAN device needs no
    privilege** and returns 102 bytes of DECnet-style counters. Each counter has a 16-bit
    header (type in bits 0–11, width in bits 13–14), from `NMADEF` in `SYS$LIB_C.TLB`.
13. **The byte counters are 32-bit, and the 16-bit "seconds since zeroed" counter latches at
    0xFFFF** (IA64 EWB0, up 12 days). LANCP SHOW DEVICE/COUNTERS shows the same counter as
    1039711, so LANCP reads a wider interface than our `$QIO`. EWB0's "bytes received" was
    3,927,661,697, which will pass 2^32 in about a day at its rate. **Phase 2 must check
    whether the 32-bit byte counters wrap or latch, and find LANCP's wider interface.**
14. TCP/IP Services (`TCPIP SHOW INTERFACE/FULL`) counts packets only, not bytes. There is
    no `netstat` in TCP/IP Services 6.0.
15. Several LAN devices with zero counters are configured but unused (IA64 EWA0, EIA0,
    EIB0). `RMA0` is also class SCOM: assigning to it fails without privilege (SS$_NOPRIV,
    36), and the `$QIO` fails with privilege. Choose interfaces by "has traffic", or by the
    `NICS` setting.

## C and DCL

16. **The default (old) STARLET headers in VSI C have no `IOSB` typedef.** Declare our own
    `struct { unsigned short status, count; unsigned int dev; }`.
17. `f$setprv(f$getjpi("","AUTHPRIV"))` overflows DCL's token limit (`%DCL-W-TKNOVF`) for an
    account with many privileges. Use `f$setprv("ALL")`.
18. **`SYS$LIB_C.TLB` is marked "HPE confidential".** Compile against it on the node
    (`CC x+SYS$LIBRARY:SYS$LIB_C.TLB/LIBRARY`), but never copy its text into this
    repository.
19. Non-blocking `connect()` returns -1 with errno EINPROGRESS (36), and `select()` then
    reports the socket writable, on both nodes (`probe_tls.c`).

## SSL3

20. SSL3 is OpenSSL **3.0.21** on x86 and **3.0.22** on IA64, through
    `SSL3$LIBSSL_SHR32`/`SSL3$LIBCRYPTO_SHR32`, with headers at `SSL3$INCLUDE:` (`define
    openssl ssl3$include:`). Both pass:
    - Ed25519 through `EVP_PKEY_new_raw_public_key` and `EVP_DigestVerify` (RFC 8032
      vectors 1 and 2, and a corrupted copy rejected);
    - SHA-256;
    - a TLS 1.3 client handshake with SNI.

    All of it works without privilege. SSL3 ships no CA store we have loaded, so verifying
    the hub's certificate needs `CA_CERT_FILE` or a bundle we choose (D5).

## Phase 2

23. **`$GETRMI` writes more than the 16-bit return length an item list provides, at least
    for `RMI$_MODES`.** With an `unsigned short` return-length variable, the agent's sample
    function crashed with an access violation at address 0x38: the caller's pointer had been
    overwritten on the stack. With 8 bytes of room it is fine on both nodes. This also
    explains the "wrong" return lengths in Phase 0 (item 5). Every return-length field we
    give a system service now has 8 bytes.
24. **The XFC size can't be read through any public interface.** `RMI$_CACHE_MEMORY` and
    `CACHE_FREE` return 0 even with `CACHE_STATE` in the same call and with all privileges.
    `RMI$_CACHE_EVERYTHING` is refused (BADPARAM) at every length from 4 to 16000.
    `$GETSYI` has no XFC size. SHOW MEMORY/CACHE knows it (190 MB on x86). D9: buff/cache
    is 0, and XFC memory counts as "used".
25. **CRTL `usleep()` returns at once for a second or more**, as POSIX allows. A 10 s
    `usleep` made two samples identical (every rate 0). Use `sleep()` for whole seconds.
26. **VSI C under `/STANDARD=C99` hides `SIGPIPE` and `localtime_r`.** Guard the first; the
    agent is single-threaded, so `localtime` serves for the second.
27. **DCL lowercases a foreign command's arguments** (`DKA200:` arrives as `dka200:`).
    `$GETDVI` doesn't care. Names typed in the config file keep their case.
28. **Cross-checks (`probes/check_metrics.com`):** the agent unprivileged, against MONITOR
    MODES over the same 10 s and SHOW MEMORY/DEVICE right after.
    - x86: busy 31.45% vs MONITOR 34.2%, with about a second's offset between the windows;
      user + super 5.34 vs 5.39; memory, swap, disks and uptime the same.
    - IA64: busy 21.88% vs 21.7%; user + super 8.69 vs 8.70; kernel + exec + interrupt
      13.19 vs 12.88; memory in use 2.32 vs 2.31 GB.

29. **On x86, idle `[7]` can advance more than `[0]` within an interval**, although `[0]`
    includes idle. With totals summed from cumulative counters, the x86 breakdown added up
    to 102.7%. The platform now builds each CPU's times from interval deltas: total =
    max(Δ[0], Δ[7]) + the other modes, interrupt = max(0, Δ[0] − Δ[7]). After the change
    the breakdown sums to exactly 100.00 (x86: 5.57 + 33.77 + 60.66), and busy 39.34%
    compares with MONITOR's 37.8% over a window about a second apart.

30. **`RMI$_CUR` counts the agent itself**: it is the current process while it asks. On an
    idle IA64 node (97% idle) the derived load average sat at about 1.0. Linux's
    `calc_load` runs in the kernel and never counts its sampler, so the agent subtracts
    itself: run queue = CUR − 1 + COM + COMO.

31. **Runs against a hub (Phase 2):** both nodes ran the agent unprivileged, 5 minutes at a
    time, against the private reference hub, which was bound to the workstation's LAN
    address for the test and then stopped. Every minute's record arrived. The CPU breakdown
    sums to 100. The load average reads 0.02–0.06 on the idle IA64 node and 0.2–0.5 on x86.
    The `%SET-W-NOTSET ... DKA800:` line in every IA64 batch log comes from that node's
    login procedure, before any of our commands run.

32. **Another process reading the agent's log can lag behind it.** TYPE showed nothing after
    21:11:56 while the agent had already logged its reconnect at 21:12:07; a second TYPE a
    few seconds later showed it. The agent calls `fflush` after every line; RMS then shows
    other readers the new records a little later. Not a problem for operators, but a test
    script that reads the log must allow a few seconds.
33. **CRTL `strerror()` text can end in spaces** ("connection refused  ").

## Linux workstation

21. **`pkill -f <pattern>` matches the shell running it** and killed the tool's own command
    (exit 144). Stop test processes by PID only. There is also a beszel agent installed on
    the workstation (`/opt/beszel-agent`) that belongs to the user: never touch it.
22. Background processes started with `setsid nohup … &` inside a tool call did not outlive
    the call. Use the tool's own background mode for long runs.
