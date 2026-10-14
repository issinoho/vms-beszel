# Phase 1: the agent core on Linux (report)

**Status:** done (2026-10-14), awaiting the user's exit review.

**Verdict:** the exit criterion is met. Our agent, built from the portable core on a Linux
stub platform, is accepted by upstream's own 0.21.0 hub binary. The system comes up with
every chart's data, the real-time view works, and a CPU alert fires and resolves. The same
core also compiles with VSI C on both nodes and passes its unit tests there.

## Exit criteria (PLAN.md)

| Criterion | Result |
|---|---|
| The local hub shows a stub system "up" with every chart filled | Pass: integration test steps 1–2 (below), plus the smoke test's `system_stats` records |
| The real-time view works | Pass: 11 replies in 10 s (step 3) |
| A CPU alert fires and resolves | Pass: triggered at 95% CPU, resolved at 5%, one resolved `alerts_history` entry (step 4) |
| Unit tests on Linux, with `-std=c99 -pedantic -Wall -Wextra` | Pass: 221 checks, no warnings, also under ASan/UBSan |
| Conformance against upstream's structs (D6) | Pass: every reply decodes strictly, goes through the hub's own decoding, and is byte-identical to upstream's encoding where map order allows |

## What was built

| Module | What it does |
|---|---|
| `src/core/bz_cbor` | The CBOR subset the hub speaks: shortest integers, 8-byte floats, byte strings, maps with counts fixed up after omissions; a bounds-checked, depth-limited decoder |
| `bz_ws` | RFC 6455 client: upgrade request with upstream's headers, `Sec-WebSocket-Accept` check, masked frames out, fragments and control frames in, protocol errors with the right close codes |
| `bz_proto` | Hub requests, and every reply in PROTOCOL.md 3.3. The SMART and monitor-sync replies are the bytes captured from upstream. |
| `bz_stats` | Upstream's statistics rules: CPU and network baselines per `CacheTimeMs`, the half-interval reply cache, Details only on fresh 60000 replies, two-decimal rounding, NICS, the Linux load-average damping, and the CombinedData encoding in upstream's field order with its omit rules |
| `bz_agent` | The connection as a pure state machine: upstream's pacing (10 s retries, 5 s floor, quick retry after a verified drop), the 120 s receive deadline, verification of the hub's Ed25519 signature before anything is answered |
| `bz_conf`, `bz_key`, `bz_url`, `bz_text` | The configuration file with overrides, `authorized_keys` parsing, `HUB_URL` (D11), base64 and friends |
| `src/plat/bz_net_ossl`, `bz_crypto_ossl` | Sockets and TLS (always verified: certificate and host name), SHA, Ed25519, randomness. The same OpenSSL code serves Linux and OpenVMS. |
| `src/plat/stub` | A 2-CPU imaginary OpenVMS host whose load follows a control file |
| `src/main/bz_main.c` | Settings as upstream reads them, fingerprint load/create, the main loop |

## Tests

1. **Unit tests** (`tests/unit/test_core.c`, `make test`, `make test-asan`): 221 checks with
   known answers from RFC 8949, RFC 6455 and the Phase 0 capture. They also drive the agent
   state machine through an upgrade, a bad and a good hub signature, cached and fresh
   GetData, ping, the deadline and the reconnect pacing.
2. **Fuzzing** (`make fuzz`): mutated hub requests, frames, HTTP responses, configuration,
   keys and URLs, and whole frames through a verified agent. 12 million rounds over four
   seeds under ASan/UBSan: no faults.
3. **Conformance** (`make conformance`, D6): a scratch copy of the pinned tree runs
   `tests/conformance` against fixtures from `build/gen_fixtures`.
   - Every fixture decodes into upstream's structs with unknown fields and duplicate keys
     rejected.
   - Each reply goes through the hub's own `transport.UnmarshalResponse` for its action.
   - Re-encoding with upstream's encoder gives exactly our bytes.
4. **Integration** (`tests/integration/itest.py`): the ASan build of the agent against the
   upstream hub binary:

   | Step | Checks | Result |
   |---|---|---|
   | 1. Connect | the hub marks the new system up | pass |
   | 2. Details and info | OS 0 with "OpenVMS E9.2-4 (x86-64)", hostname, cores, memory, version 0.21.0, WebSocket, extra filesystem | pass |
   | 3. Real time | 11 replies in 10 s | pass |
   | 4. Alert | a CPU > 80% alert fires at 95% and resolves at 5%; one resolved history entry | pass |
   | 5. Restart | clean stop on SIGTERM, the hub marks it down, it comes back up, same fingerprint | pass |
   | 6. Wrong KEY | the signature is refused, nothing answered, the system is not marked up | pass |
   | 7. Sanitizers | no ASan, LeakSanitizer or UBSan report from any of the three agent runs | pass |

   21 checks, 0 failures (`out/itest-run.log`; about 8 minutes).

5. **VSI C** (`build_vms.com TEST`): the core, the SSL3 crypto and the unit tests build
   with no diagnostics, and all 221 checks pass, on x86-64 (VSI C 7.7-003) and IA64 (VSI C
   7.4-001).

## Settled along the way

- PROTOCOL Q4 is moot and Q5 is settled (ZfsData uses its `json` key `"complete"`).
- In the 0.21.0 hub, alert records can be listed by their owner but read by ID only by a
  superuser. This only affected the test script.

## For the exit review

| # | Question | Recommendation |
|---|---|---|
| D9 correction | The hub's web page draws the root disk I/O chart even with no data, so OpenVMS systems show a flat zero line there. The "more I/O" sheet stays hidden. | Accept for v1. The alternative, inventing bytes from operation counts, breaks D9's rule. Revisit if Phase 2 finds XFC byte statistics. |
| D11 | Three deliberate departures from upstream: `HUB_URL` must be http/https (upstream silently downgrades `wss://` to plain text); `NICS` is case-insensitive; NIC selection is re-evaluated on every sample | Accept. |

## Next: Phase 2

The VMS platform layer (`src/plat/vms`) from the Phase 0 probes, the agent image in
`build_vms.com`, and every metric cross-checked against MONITOR and SHOW output on both
nodes. Two Phase 2 checks are already waiting: the IA64 LAN counter about to pass 2^32,
and the XFC size.
