# vms-beszel protocol specification (beszel 0.21.1, WebSocket mode)

This document describes the protocol between a beszel **agent** and an **unmodified beszel hub
0.21.1**, as far as an agent that only uses WebSocket needs it. Sections 1-9 were written from
upstream 0.21.0, and their `file:line` references are relative to that tree (`tools/fetch.sh`
at commit 97ed300 fetches it). **Section 10 lists what 0.21.1 changed**, with references to the
pinned tree `cache/beszel-0.21.1`. Where section 10 and an earlier section disagree, section 10
is right.

Our agent reports agent version `0.21.1`, `Os = 0` (Linux) and `OsName = "OpenVMS <version>"`.
It never listens for SSH.

Two library sources are **not** in the tree: `github.com/lxzan/gws v1.10.2` (WebSocket) and
`github.com/fxamacker/cbor/v2 v2.9.4` (CBOR) (`go.mod:9`, `go.mod:12`). Statements that depend on
their default behaviour are marked **[lib]**. They are repeated in section 8 (Open questions),
with what would settle each one. The same applies to `golang.org/x/crypto/ssh`
(`go.mod:21`) and `github.com/blang/semver v3.5.1` (`go.mod:6`).

Conventions:
- "B/s" means bytes per second, as an integer.
- "GiB" means bytes / 1073741824. "MiB" means bytes / 1048576. Beszel calls these "GB" and
  "MB" in its code and UI, but always divides by powers of 1024 (`agent/utils/utils.go:25-32`).
- "2dp" means rounded with `math.Round(x*100)/100` (`agent/utils/utils.go:35-37`). That is
  round half away from zero at the second decimal place.
- `SITE/` stands for `internal/site/src/` (the hub's web UI source).

---

## 1. Connection

### 1.1 URL

- The agent reads `HUB_URL` and parses it as a URL. It is an error if there is no host
  (`agent/client.go:72-82`).
- The scheme becomes `wss` **only if** the HUB_URL scheme is `https`. **Every other scheme
  becomes `ws`**, including `wss` (`agent/client.go:174-178`). So `HUB_URL=wss://hub` gives a
  plain-text connection. Mirror this, or reject anything other than `http`/`https` at startup
  (our choice; document it).
- The path is `path.Join(<HUB_URL path>, "api/beszel/agent-connect")` (`agent/client.go:179`).
  `path.Join` cleans the path: repeated slashes and a trailing `/` disappear, and a hub under a
  sub-path (`https://x/beszel/`) gives `/beszel/api/beszel/agent-connect`.
- Any query string in HUB_URL is kept, because `url.String()` is used for the address
  (`agent/client.go:187`).
- The hub serves this route as `GET /api/beszel/agent-connect`, without authentication
  (`internal/hub/api.go:194`).

### 1.2 Upgrade request headers sent by the agent

The upstream agent adds exactly three headers to the WebSocket handshake
(`agent/client.go:189-193`):

| Header | Value | Hub use |
|---|---|---|
| `X-Token` | the token (TOKEN or TOKEN_FILE, section 7) | required: non-empty and at most 64 bytes (`internal/hub/agent_connect.go:141-146`) |
| `X-Beszel` | `0.21.0` (`beszel.Version`, `beszel.go:9`) | required: non-empty, then parsed as semver (`internal/hub/agent_connect.go:87`) |
| `User-Agent` | `Mozilla/5.0 (Windows NT 11.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36` or the same with `Macintosh; Intel Mac OS X 14_0_0`, chosen by whether `time.Now().UnixNano()` is even (`agent/client.go:414-424`) | not read by the hub. It exists "to avoid being blocked by Cloudflare or other anti-bot measures" (`agent/client.go:412-413`). |

The standard RFC 6455 headers (`Host`, `Upgrade`, `Connection`, `Sec-WebSocket-Key`,
`Sec-WebSocket-Version: 13`) are added by gws **[lib]**. The agent's `gws.ClientOption` sets
no subprotocol and no compression (`agent/client.go:186-197`).

**HUB_HEADERS does not exist in 0.21.0** (0.21.1 adds it: section 10). No file in the tree mentions it (a search of the
whole tree for `HUB_HEADERS` finds nothing), and the only request headers are the three above.
DECISIONS.md D5 lists `HUB_HEADERS`. It must either be dropped or be documented as a vms-beszel
extension that upstream does not have.

Proxy: if `BESZEL_AGENT_ALL_PROXY` is set, it is copied to `ALL_PROXY`
(`agent/client.go:182-184`), and the dialer is `proxy.FromEnvironment()`
(`agent/client.go:194-196`). That is SOCKS5 from `golang.org/x/net/proxy` **[lib]**. This is out
of scope for v1 (DECISIONS.md D4).

### 1.3 Hub checks and HTTP responses

`agentConnect` runs these checks in order (`internal/hub/agent_connect.go:59-101`):

| # | Check | Failure response |
|---|---|---|
| 1 | `X-Token` and `X-Beszel` both non-empty, and token length ≤ 64 (`:140-148`) | **400**, empty body (`:62-65`) |
| 2 | The token is a universal token: the in-memory map of 1-hour tokens (`:68`), or a permanent token in the `universal_tokens` table with a user (`:69-77`) | (no failure here) |
| 3 | There are `fingerprints` rows with this token (`:80`, `:160-169`), or the token is universal | **401**, body `Invalid token` (`:81-84`) |
| 4 | `semver.Parse(X-Beszel)` succeeds | **401**, body `Invalid agent version` (`:87-90`) |
| 5 | gws upgrade succeeds | **500**, body `WebSocket upgrade failed` (`:93-96`). gws may already have written its own response here **[lib]**. |

Then the hub starts `verifyWsConn` in a goroutine (`:98`) and the handler returns.

**X-Beszel parsing.** It uses `blang/semver` `Parse` (`internal/hub/agent_connect.go:87`)
**[lib]**. To our knowledge Parse is strict: exactly `MAJOR.MINOR.PATCH`, no leading `v`, no
leading zeros, with optional `-prerelease` and `+build`. Send exactly `0.21.0`, for three
reasons:

- the parsed version sets the hub's feature gates (section 3.1);
- the UI compares `info.v` with the hub version for the green "up to date" colour
  (`SITE/components/systems-table/systems-table-columns.tsx:524-531`);
- `parseSemVer` in the UI cuts at `-` and calls `Number()` on each part
  (`SITE/lib/utils.ts:396-405`).

### 1.4 WebSocket parameters

Hub side:

- The upgrader is `gws.NewUpgrader(handler, &gws.ServerOption{})`, with all defaults
  (`internal/hub/ws/ws.go:47-54`). No compression, subprotocol, maximum payload size or
  origin check is set in source.
  - **permessage-deflate:** gws only negotiates it when `PermessageDeflate.Enabled` is set, which
    it is not **[lib]**. So the hub does not compress, and our agent must not offer the extension
    (upstream doesn't either: `agent/client.go:186-197`). Compression is neither required nor used.
  - **Subprotocols:** none on either side.
  - **Maximum message size:** the gws default read limit **[lib]**. Our messages are a few KB.
- **Frame type.** The hub sends every request as one binary message (`OpcodeBinary`)
  (`internal/hub/ws/request_manager.go:89-100`). It **ignores** any message that is not binary
  or is empty (`internal/hub/ws/ws.go:75-77`). Our responses must be **binary** frames.
- **Who sends pings.**
  - The hub sends a ping only for a **paused** system, once per update tick (60 s)
    (`internal/hub/systems/system.go:237-248`, `internal/hub/ws/ws.go:115-122`).
  - Otherwise the hub sends no pings. The 60 s `GetData` request is the keep-alive.
  - The upstream agent never sends pings. It answers the hub's pings with a pong that carries
    the same payload (`agent/client.go:274-277`).
- **Hub deadline: 70 s** (`internal/hub/ws/ws.go:19-21`).
  - Set on open (`:68-70`), on every message received (`:73-74`), and when the hub pings
    (`:120`).
  - It is a `net.Conn` deadline covering reads and writes **[lib]**.
  - If the agent sends nothing for 70 s, the hub's read loop fails and the connection closes.
    Answering every `GetData` (every 60 s) keeps the connection open.
- **Agent deadline: 120 s** (`agent/client.go:29-33`). Set on open (`:225-227`), on every message
  (`:253`) and on every ping (`:275`). Mirror it: if nothing arrives from the hub for 120 s, drop
  the connection and reconnect.
- **Close codes.**
  - When the hub rejects a connection after the upgrade, it sends close code **1000**, with the
    error text as the reason: `fingerprint mismatch`, `no matching fingerprints`,
    `invalid token`, `context deadline exceeded`, … (`internal/hub/agent_connect.go:112-116`,
    `internal/hub/ws/ws.go:105-112`).
  - A plain close (system removed, token rotated, transport error) is also 1000 with no reason
    (`internal/hub/transport/websocket.go:70-74`, `internal/hub/systems/system.go:1055-1059`).
  - The agent closes with 1000 and no reason (`agent/client.go:332-336`). It also closes with
    1000 after any failed write (`agent/client.go:387-392`).
- **Message order and concurrency.**
  - The hub may have several requests outstanding at once: the 60 s update, the 1 s real-time
    poll and a monitor sync. Each request carries a different `Id`
    (`internal/hub/ws/request_manager.go:46`, `:70-74`).
  - gws calls `OnMessage` from the read loop one message at a time, so the upstream agent
    answers requests in order **[lib]**.
  - Answering out of order is allowed: the hub matches replies by `Id`
    (`internal/hub/ws/request_manager.go:103-136`).
- **Fragmentation and masking.** These are standard RFC 6455 and handled by gws **[lib]**.
  - Client frames must be masked.
  - Our client must accept fragmented messages, although the hub writes each message with a
    single `WriteMessage` call (`internal/hub/ws/request_manager.go:99`).

### 1.5 What the hub does after the upgrade

`verifyWsConn` (`internal/hub/agent_connect.go:105-137`):

1. It starts the read loop (`:118`).
2. It signs the token and sends `CheckFingerprint` (section 2), waiting at most **5 s** for the
   reply.
   - The context is `context.Background()` (`:125`), so `SendRequest` applies its default 5 s
     timeout (`internal/hub/ws/request_manager.go:51-56`).
3. It binds the fingerprint (section 2.5).
4. `AddWebSocketSystem` (`internal/hub/systems/system_manager.go:348-368`):
   - replaces any existing in-memory system object (the old one's context is cancelled);
   - starts the updater;
   - in a goroutine, sends `SyncNetworkMonitors` (`:364-365`).
5. The updater sends a first `GetData` with `IncludeDetails = true` **immediately**
   (`internal/hub/systems/system.go:128-134`, `:171-173`).
   - A second `GetData` follows after a random jitter of 51–95 % of 60 s
     (`:148-157`, `:1064-1070`).
   - After that, one arrives every 60 s (`:136`, `:144-147`).

If any step fails, the hub closes the WebSocket with code 1000 and the error text as the reason
(`internal/hub/agent_connect.go:112-116`).

When the connection closes, the hub waits **5 s** before marking the system down
(`internal/hub/ws/ws.go:87-102`, `internal/hub/systems/system.go:148-151`). A reconnect within
5 s replaces the system object (step 4), so the system never shows "down".

---

## 2. Handshake (CheckFingerprint)

### 2.1 Request

The hub sends `HubRequest{Action: 1, Data: FingerprintRequest, Id}`
(`internal/hub/ws/handlers.go:60-63`). The layouts are in section 5.4.

`FingerprintRequest` (`internal/common/common-ws.go:57-60`):

| Key | Field | Go type | Omit rule | Meaning |
|---|---|---|---|---|
| 0 | Signature | `[]byte` | never omitted | SSH signature blob (below) |
| 1 | NeedSysInfo | `bool` | never omitted | true if the token is a universal token (`internal/hub/agent_connect.go:125`) |

### 2.2 What is signed and how

- The message is the token bytes exactly as sent in `X-Token`, with no terminator
  (`internal/hub/ws/handlers.go:54`: `challenge := []byte(token)`).
- The hub signs with its SSH key: `signer.Sign(nil, challenge)` (`:55`). The key is the file
  `id_ed25519` in the hub data directory, generated as Ed25519 if missing
  (`internal/hub/hub.go:152-197`).
- Only `signature.Blob` is sent (`internal/hub/ws/handlers.go:61`). The format name
  (`ssh-ed25519`) is **not** sent.
- For an Ed25519 key, x/crypto/ssh puts the **raw 64-byte Ed25519 signature** (R‖S, RFC 8032)
  in `Blob`, with no SSH wire-format wrapping **[lib]**. So the agent verifies with plain
  Ed25519: message = token bytes, signature = the 64 bytes of key 0, public key = the 32 bytes
  from the `ssh-ed25519` authorized key.
- A hub whose `id_ed25519` file holds a non-Ed25519 key (the file is parsed with the generic
  `ssh.ParsePrivateKey`, `internal/hub/hub.go:166`) would sign in another format. We support
  Ed25519 only, and must log a clear error if KEY is any other type.

### 2.3 How the agent checks it

- `verifySignature` tries every configured public key in turn. For each one it builds
  `ssh.Signature{Format: pubKey.Type(), Blob: signature}` and calls `pubKey.Verify(token, sig)`.
  The first success wins (`agent/client.go:317-328`).
- If none verifies, the error is `invalid signature - check KEY value` (`:327`).
- **On failure the upstream agent sends nothing.**
  - `handleAuthChallenge` returns the error (`agent/client.go:286-288`).
  - `OnMessage` only logs it: `slog.Error("Error handling message", ...)`
    (`agent/client.go:267-269`).
  - The hub times out after 5 s and closes with 1000 `context deadline exceeded`
    (section 1.5).
  - Mirror this: do not reply, log the error, and let the hub close.
- **Keys** (`agent/server.go:235-251`, loaded in `internal/cmd/agent/agent.go:121-143`):
  - Source order: `-key` flag, then env `KEY` if non-empty, then the contents of `KEY_FILE`.
    If none is set, the agent exits with "no key provided" (`internal/cmd/agent/agent.go:19-23`,
    `:207`).
  - The input is split into lines. Each line is trimmed; empty lines and lines starting with
    `#` are skipped.
  - Every other line must parse with `ssh.ParseAuthorizedKey`. That is authorized_keys format:
    optional options, key type, base64 key, optional comment **[lib]**.
  - **One bad line is a fatal error** (`agent/server.go:244-247`).
  - **Several keys are allowed**, one per line.
  - Our implementation: parse `ssh-ed25519 <base64> [comment]`, with optional leading
    authorized_keys options. Base64-decode the key and check the SSH wire format: string
    `"ssh-ed25519"`, then a 32-byte string.

### 2.4 Response

After a good signature, the agent:

1. sets `hubVerified = true` (`agent/client.go:290-296`);
2. signals "WebSocket connected" to the connection manager (`:297`);
3. replies with `FingerprintResponse` (`:299-313`).

`FingerprintResponse` (`internal/common/common-ws.go:62-68`):

| Key | Field | Go type | Omit rule | Value from upstream agent |
|---|---|---|---|---|
| 0 | Fingerprint | `string` | never omitted | the agent fingerprint (2.6) |
| 1 | Hostname | `string` | omitzero (omit if `""`) | only if NeedSysInfo: `systemDetails.Hostname` (`os.Hostname()`, `agent/system.go:44`) |
| 2 | Port | `string` | omitzero | only if NeedSysInfo: the port part of the SSH listen address (`agent/client.go:306-307`), default `"45876"` (`agent/server.go:256-272`) |
| 3 | Name | `string` | omitzero | only if NeedSysInfo: env `SYSTEM_NAME` (`agent/client.go:304`) |

**It must be sent in the legacy field `AgentResponse` key 2 (`Fingerprint`), not in key 7
(`Data`).**

- The agent puts it there (`agent/response.go:18-19`).
- The hub's `fingerprintHandler.Handle` reads only `agentResponse.Fingerprint` and fails with
  `no fingerprint data in response` otherwise (`internal/hub/ws/handlers.go:40-46`).
- The generic `Data` fallback in `transport.UnmarshalResponse` is **not** used for this action.
  The handshake goes through `WsConn.handleAgentRequest` (`internal/hub/ws/ws.go:139-166`).

If the response carries `Error` (key 3), the hub fails with that text and closes
(`internal/hub/ws/ws.go:158-160`).

**Universal tokens** (NeedSysInfo = true). The hub creates the system with
(`internal/hub/agent_connect.go:267-291`):

- name = Name, else Hostname, else the remote IP;
- host = the remote IP, always (CF-Connecting-IP, X-Real-IP, the first X-Forwarded-For entry,
  then RemoteAddr: `:316-337`);
- port = Port, else `"45876"`;
- users = the token's user.

Send Port `"45876"` (or the port from LISTEN/PORT, section 7), as upstream does. It is only
stored, because we never listen.

### 2.5 Fingerprint binding on the hub

`findOrCreateSystemForToken` (`internal/hub/agent_connect.go:173-186`):

| Situation | Result |
|---|---|
| No fingerprint rows; universal token with a user | create a new system and a fingerprint row (`:190-198`, `:241-264`) |
| No rows; not universal, or no user | error `no matching fingerprints` (`:193-195`), then close |
| Exactly 1 row; not universal; stored fingerprint empty | store ours, accept (first connection) (`:204-211`) |
| 1 row; stored fingerprint equals ours | accept (`:218`) |
| 1 row; stored fingerprint differs | error **`fingerprint mismatch`** (`:213-216`), then close |
| Several rows, or universal token | accept the row whose fingerprint equals ours (`:225-229`). Otherwise, if universal, create a new system (`:233-235`); if not, `fingerprint mismatch` (`:237`). |

Notes:

- Comparison is exact string equality (`:214`, `:226`). The hub does not validate the
  fingerprint's length or form.
  - The schema says min 9, max 255 characters, but `SetFingerprint` uses `SaveNoValidate`, so
    that is not enforced (`internal/hub/agent_connect.go:294-312`).
  - Any non-empty stable string works. We keep upstream's 48 hex digits.
- When the user changes the system's fingerprints row (rotating the token or clearing the
  fingerprint), the hub sets the system down and closes its WebSocket
  (`internal/hub/systems/system_manager.go:158-171`).
  - A rotated token then fails with 401 on every retry.
  - A cleared fingerprint is re-bound to ours at the next connection.
- If `AddWebSocketSystem` cannot find the system record, the hub closes the connection
  (`internal/hub/agent_connect.go:136`, `internal/hub/systems/system_manager.go:349-352`).

### 2.6 Fingerprint generation and storage (agent)

- `GetFingerprint(dataDir, hostname, cpuModel)` (`agent/fingerprint.go:27-38`):
  - If dataDir is set and `<dataDir>/fingerprint` reads successfully, its trimmed contents are
    used.
  - Otherwise a fingerprint is generated and saved, ignoring any save error.
- Generation (`agent/fingerprint.go:43-62`):
  - The seed is `host.HostID()` from gopsutil (a machine UUID) **[lib]**.
  - If that fails, is empty, or equals the known bad UUID
    `03000200-0400-0500-0006-000700080009` (`:18`), the seed is `hostname + cpuModel`.
  - The fingerprint is `hex(SHA-256(seed)[0:24])`: **48 lowercase hex digits**.
- File (`agent/fingerprint.go:15`, `:65-80`):
  - Name `fingerprint` (no extension) in the data directory.
  - Content is the fingerprint with **no trailing newline**, mode 0644.
  - When read, surrounding whitespace is trimmed, and an empty file counts as missing.
- Reset: the `fingerprint reset` subcommand deletes the file (`internal/cmd/agent/agent.go:171-179`).
- VMS: `BESZEL$ROOT:[DATA]FINGERPRINT.` (PLAN.md).
  - The seed is our choice, since gopsutil HostID has no VMS meaning. A good one is SCSNODE +
    SCSSYSTEMID + HW_NAME.
  - Accept and trim an existing file, so a fingerprint copied from elsewhere works.

### 2.7 Before the hub is verified

`HandlerRegistry.Handle` (`agent/handlers.go:73-88`):

- An action with no registered handler fails with `unknown action: N` (`:75-77`).
- Any action other than `CheckFingerprint` fails with **`hub not verified`** while
  `hubVerified` is false (`:80-82`).
- Over WebSocket, **both errors are only logged; nothing is sent**
  (`agent/client.go:267-269`).
- `hubVerified` resets to false on every new connection and on close
  (`agent/client.go:213-216`, `:237-238`).
- The hub only sends other actions after the handshake, so in practice this guards against a
  hub that cannot prove its key.

---

## 3. Actions

### 3.1 Envelope

`HubRequest` (`internal/common/common-ws.go:37-41`):

| Key | Field | Go type | Omit rule |
|---|---|---|---|
| 0 | Action | `uint8` | never |
| 1 | Data | any (hub) / raw CBOR (agent) | omitempty + omitzero: absent when the hub passes `nil` (GetSmartData, GetPackageUpdates: `internal/hub/systems/system.go:863`, `:872`) |
| 2 | Id | `*uint32` | omitempty: absent if nil. The 0.21.0 hub always sets it. Ids start at 1 and count up per connection (`internal/hub/ws/request_manager.go:46`, `:70-74`). |

`AgentResponse` (`internal/common/common-ws.go:44-55`):

| Key | Field | Go type | Omit rule | Used by a 0.21.0 agent for |
|---|---|---|---|---|
| 0 | Id | `*uint32` | omitempty | always: echo the request's Id |
| 1 | SystemData | `*CombinedData` | omitempty, omitzero | GetData (`agent/response.go:16-17`) |
| 2 | Fingerprint | `*FingerprintResponse` | omitempty, omitzero | CheckFingerprint (`agent/response.go:18-19`) |
| 3 | Error | `string` | omitempty, omitzero | never over WS by upstream (see below) |
| 4 | String | `*string` | omitempty, omitzero | container logs/info, systemd logs (`agent/response.go:20-21`) |
| 5 | SmartData | `map[string]SmartData` | omitempty, omitzero | GetSmartData (`agent/response.go:22-26`) |
| 6 | ServiceInfo | `systemd.ServiceDetails` | omitempty, omitzero | GetSystemdInfo (`agent/response.go:27-28`) |
| 7 | Data | `cbor.RawMessage` | omitempty, omitzero | every other payload type (`agent/response.go:29-31`): ZFS, package updates, monitor sync |
| 8 | SmartComplete | `bool` | omitempty, omitzero | GetSmartData |

**Data (key 7) holds the CBOR data item itself, not a byte string wrapping it.**
`cbor.RawMessage` is written verbatim **[lib]**. The hub copies it as raw bytes and decodes it
again into the destination type (`internal/hub/transport/transport.go:37-41`).

Reply rules:

- **Always set `Id`.** If a request ever arrives without `Id`, upstream sends the bare payload
  with no envelope (`agent/client.go:403-410`). The 0.21.0 hub never does this.
- **The hub's dispatch** (`internal/hub/transport/websocket.go:27-62`, used for every action
  except CheckFingerprint):
  - Decode the AgentResponse. A decode error is returned (`:48-51`).
  - If `Error` is non-empty, return it as the error (`:53-55`).
  - Otherwise `UnmarshalResponse`: if `len(Data) > 0`, decode Data into the destination
    (`internal/hub/transport/transport.go:37-41`). Otherwise use the legacy field for the
    action (`:48-130`).
  - SyncNetworkMonitors, GetZfsData and GetPackageUpdates have **no legacy field**, so a
    reply without Data fails with `unsupported action: N` (`:129`).
- **Routing** (`internal/hub/ws/request_manager.go:103-136`):
  - An Id that is not pending (for example, it already timed out) is dropped silently
    (`:122-125`).
  - A reply that fails to decode as AgentResponse, or has no Id, goes to the **oldest** pending
    request (`:105-113`, `:139-163`).
- **Timeouts:** each action's timeout is listed below. Without a caller deadline the default is
  5 s (`internal/hub/ws/request_manager.go:51-56`).
- **What a WebSocket error means on the hub:**
  - For **GetData**, any error other than a timeout makes the hub **close the WebSocket** and try
    SSH (`internal/hub/systems/system.go:781-790`).
  - For **every other action**, `System.request` (`internal/hub/systems/system.go:681-700`)
    returns an `Error` reply to the caller and does nothing else (`:687-688`).
  - A **timeout** (`context.DeadlineExceeded`) makes the hub try SSH to `host:port`
    (`:702-713`, `:694-699`). It dials with a 4 s timeout
    (`internal/hub/systems/system_manager.go:38-39`, `:405-418`), unless the hub sets
    `DISABLE_SSH=true` (`internal/hub/systems/system.go:69-76`, `:726-729`).
  - So for anything except GetData, a **prompt Error reply is cleaner than silence**. It avoids
    a 5–60 s wait and a pointless SSH dial to the VMS host.
- **Upstream sends nothing on a handler error over WS.** `OnMessage` logs it
  (`agent/client.go:267-269`). Only the SSH path encodes `AgentResponse{Error}`
  (`agent/server.go:178-205`).

Version gates on the hub. With `X-Beszel: 0.21.0`, all of these apply:

- ≥ 0.13.0: AgentResponse envelope (`beszel.go:18`, `internal/hub/ws/ws.go:149`,
  `internal/hub/transport/websocket.go:44`);
- ≥ 0.18.9: ZFS detail requests (`beszel.go:21`, `internal/hub/systems/system_zfs.go:23-25`);
- ≥ 0.20.0: network monitor sync (`beszel.go:24`,
  `internal/hub/systems/network_monitors.go:66-69`).

### 3.2 Action table

Action numbers come from `internal/common/common-ws.go:12-34`. Handlers are in
`agent/handlers.go:48-65`.

| # | Action | When the 0.21.0 hub sends it | Request Data | Hub timeout |
|---|---|---|---|---|
| 0 | GetData | at connection, after the jitter, then every 60 s (1.5). Every 1 s per system while a UI client watches the 1-minute (real-time) chart (`internal/hub/systems/system_realtime.go:141-175`). | `DataRequestOptions` (4.1) | 30 s (`internal/hub/systems/system.go:801`, `:807`) |
| 1 | CheckFingerprint | once per connection | `FingerprintRequest` | 5 s |
| 2 | GetContainerLogs | the user opens a container row (`SITE/components/containers-table/containers-table.tsx:281`, `:362-373`). Only shown if the system has containers. | `{0: ContainerID string}` (`internal/common/common-ws.go:79-81`) | 5 s (`internal/hub/systems/system.go:828`) |
| 3 | GetContainerInfo | same as 2 (`containers-table.tsx:303`) | `{0: ContainerID string}` (`:83-85`) | 5 s (`system.go:819`) |
| 4 | GetSmartData | **automatic**, from the hub updater, once details have been received and then hourly (`internal/hub/systems/system.go:206-218`, `internal/hub/systems/system_smart.go:46-63`). Also manual "Refresh" on a SMART row (`SITE/components/routes/system/smart-table.tsx:423`). | none (key 1 absent) | 60 s (`system.go:855`) |
| 5 | GetSystemdInfo | the user opens a systemd service (`SITE/components/systemd-table/systemd-table.tsx:322`). The hub first requires a `systemd_services` row (`internal/hub/api.go:437-444`). | `{0: ServiceName string}` (`common-ws.go:87-89`) | 5 s (`system.go:837`) |
| 6 | GetZfsData | **automatic**, once details have been received and then hourly (`system.go:220-232`). Also manual refresh (`internal/hub/api.go:531-547`). | `ZfsDataRequest{0: Force bool, omitempty}` (`common-ws.go:75-77`) | 60 s (`system.go:878`) |
| 7 | SyncNetworkMonitors | **automatic**, right after connection (`system_manager.go:364-365`) and after the next good GetData while a sync is pending (`system.go:777-779`, `internal/hub/systems/network_monitors.go:15-23`). Also when the user creates, edits, pauses or deletes a monitor on this system (`internal/hub/network_monitors.go:41-106`). | `monitor.SyncRequest` (3.4) | 5 s, or 15 s for an upsert with RunNow (`internal/hub/systems/network_monitors.go:70-76`) |
| 8 | GetPackageUpdates | the package-updates table mounts. That table exists only if `info.pu[0] > 0` (`SITE/components/routes/system/package-updates-table.tsx:198`, `:211`). We never send `pu`. | none | 5 s (`system.go:869`) |
| 9 | GetSystemdLogs | the user opens a service **and** `info.jl` is true (`internal/hub/api.go:476-483`). We never send `jl`, so the hub answers `""` itself. | `{0: ServiceName string}` (`common-ws.go:91-93`) | 5 s (`system.go:846`) |

### 3.3 What to reply to each action

"Upstream on a bare host" describes what the 0.21.0 agent sends on a Linux machine without
Docker, smartctl, systemd, ZFS/btrfs or a package manager.

| # | Upstream on a bare host | Hub reaction | **vms-beszel reply** |
|---|---|---|---|
| 0 | `{0:Id, 1:CombinedData}` | records, status "up" (section 4) | `{0:Id, 1:CombinedData}`. **Never reply Error to GetData.** The hub would close the WebSocket, try SSH, fail and mark the system down with an `Error` log "System down" (`internal/hub/systems/system.go:781-790`, `:145-146`, `:664-666`). A missed 30 s deadline also gives "System down", but keeps the WS open (`:783-785`). |
| 1 | `{0:Id, 2:FingerprintResponse}`, or nothing on a bad signature | 2.5 | the same |
| 2, 3 | `dockerManager` exists but Docker is not running, so the handler fails and **nothing is sent** (`agent/handlers.go:130-147`); the hub times out and tries SSH. With `DOCKER_HOST=""` (manager nil) it sends `String ""` (`:131-133`). | `""` → the hub returns `{"logs": ""}` / `{"info": ""}` (`internal/hub/api.go:403-408`) | **`{0:Id, 4:""}`**. This is silent: no hub log. The UI never asks, because we report no containers. |
| 4 | smartManager is nil, so it sends `SmartDataResponse{Data: {}}` through the legacy fields (`agent/handlers.go:181-183`, `agent/response.go:24-26`). The empty map is **omitted** (omitempty), so the wire carries `{0:Id}` only. | Data empty, so the legacy path runs; SmartData is nil, so the error is `no SMART data in response` (`internal/hub/transport/transport.go:98-101`). Logged at **Info**: `SMART fetch result ... success=false err=...`, hourly (`internal/hub/systems/system_smart.go:20-24`, `:33-43`). | **`{0:Id, 7:{0:{}}}`**: Data = SmartDataResponse with an empty map. The hub decodes it, saves nothing (`system_smart.go:67-70`) and logs at Info `SMART fetch result success=false devices=0 err=<nil>` hourly. Both forms only log at Info; ours has no error text. Byte-for-byte mirroring (`{0:Id}`) is also acceptable. The 2-minute cooldown/retry logic is the same either way (`system_smart.go:42`). |
| 5 | systemdManager is nil, so it returns `errors.ErrUnsupported` and **nothing is sent** (`agent/handlers.go:233-235`) | hub timeout, then SSH, then 500 to the UI | **`{0:Id, 3:"unsupported operation"}`** (the text of `errors.ErrUnsupported`). The hub returns 500 to that UI call (`internal/hub/api.go:445-448`). Never triggered in practice: there are no `systemd_services` rows for us. |
| 6 | `GetDetail` → `ZfsData{Complete: true, CompleteBackends: ["zfs","btrfs"]}` in Data (`agent/handlers.go:200-209`, `agent/storage_pool.go:310-322`) | `CanRefreshPool` is true, nothing to save, `Complete` is true, so there is no error. Info log `ZFS fetch result success=false pools=0`, hourly (`internal/hub/systems/system_zfs.go:29-56`, `:80-129`). | **`{0:Id, 7:{"complete": true, "completeBackends": ["zfs","btrfs"]}}`**. ZfsData has no `cbor` tags, only `json` tags (`internal/entities/zfs/zfs.go:7-12`), so upstream's keys are **text strings**. That assumes fxamacker falls back to the `json` tag name **[lib]** (Open question Q5). A safe alternative is `7: null` (0xF6): the hub then logs Info `err=incomplete ZFS pool inventory` (`system_zfs.go:81-83`) and nothing else happens. |
| 7 | Replace: `SyncResponse{}` in Data (`agent/network_monitor.go:73-77`). Upsert/Delete: a result or an empty response. | Replace: result ignored. Upsert: a zero `Result` means "no result" (`internal/hub/systems/network_monitors.go:41-55`). | **`{0:Id, 7:{0:{8:0,10:0,11:0,12:0}}}`**: upstream's exact bytes (section 9). An empty map `{}` decodes the same, but we mirror upstream. **Must succeed.** An Error reply to the replace sync makes the hub log **Warn `failed to sync monitors to agent`** and retry after **every** GetData, i.e. a Warn every minute forever (`internal/hub/systems/network_monitors.go:15-23`, `internal/hub/systems/system.go:777-779`). Error to an upsert or delete logs Warn once per user edit (`internal/hub/network_monitors.go:94-96`, `:101-104`). Monitors are then accepted but never run; see Open question Q9. |
| 8 | packageUpdates nil (not Linux, or disabled) → `PackageUpdates{}` in Data = `{3: null}` (`agent/handlers.go:219-221`, `internal/entities/system/package_updates.go:13-20`) | the JSON `{"packages": null}` goes to the UI | **`{0:Id, 7:{3: null}}`** (mirror), or `{3: []}`. Silent. Never triggered, since `pu` is never sent. |
| 9 | like 5 | — | **`{0:Id, 3:"unsupported operation"}`**. Never triggered (above). |
| ≥10 | `unknown action: N` logged, nothing sent (`agent/handlers.go:75-77`) | — | **`{0:Id, 3:"unknown action: N"}`** and a log line. The 0.21.0 hub never sends these. |
| any, before verification | `hub not verified` logged, nothing sent | — | the same: nothing sent, log a warning |

**Nothing shows in the UI for any of these replies.** The tables that would call actions 2–9
only render when data we never send exists (see section 4.8). The only hub log lines our agent
causes in normal running are the two hourly **Info** lines for SMART and ZFS. These come from
the hub's own background fetch, and an upstream agent on a bare host causes them too.

### 3.4 Request and response payload structs for the other actions

`monitor.SyncRequest` (`internal/entities/monitor/monitor.go:41-46`):

| Key | Field | Type | Omit rule |
|---|---|---|---|
| 0 | Action | uint8: 0 = replace all, 1 = upsert, 2 = delete (`:11-18`) | never |
| 1 | Config | `Config` | omitempty |
| 2 | Configs | `[]Config` | omitempty |
| 3 | RunNow | bool | omitempty |

`monitor.Config` (`internal/entities/monitor/monitor.go:21-31`):

| Key | Field | Type | Omit rule |
|---|---|---|---|
| 0 | ID | string | never |
| 1 | Target | string | never |
| 2 | Protocol | string: `icmp`, `tcp`, `http` or `dns` | never |
| 3 | Port | uint16 | omitempty |
| 4 | Interval | uint16, seconds | never |
| 5 | Server | string | omitempty |

`SyncResponse` is `{0: Result, omitempty}` (`internal/entities/monitor/monitor.go:49-51`). Result is
at `:70-91`.

`smart.SmartDataResponse` is `{0: map[string]SmartData, 1: Complete bool, omitempty}`
(`internal/entities/smart/smart.go:536-539`).

`system.PackageUpdates` (`internal/entities/system/package_updates.go:13-20`):

| Key | Field | Type | Omit rule |
|---|---|---|---|
| 0 | Manager | string | omitempty |
| 1 | CheckedAt | int64 | omitempty |
| 2 | SecurityKnown | bool | omitempty |
| 3 | Packages | `[]PackageUpdate` | never |

Our agent must decode SyncRequest only enough to accept it. Strictly, we can reply without
decoding anything but the envelope.

---

## 4. GetData in depth

### 4.1 DataRequestOptions

`internal/common/common-ws.go:70-73`:

| Key | Field | Go type | Omit rule |
|---|---|---|---|
| 0 | CacheTimeMs | `uint16` | never |
| 1 | IncludeDetails | `bool` | never |

Values the 0.21.0 hub sends (the complete list; these are the only two places that build the
struct):

| CacheTimeMs | IncludeDetails | Source |
|---|---|---|
| **60000** | true until a reply containing Details has been saved, then false. It becomes true again after the system goes down (`internal/hub/systems/system.go:667`). | the updater (`internal/hub/systems/system.go:167-173`, `interval` = 60000 at `internal/hub/systems/system_manager.go:35`) |
| **1000** | false | real-time worker (`internal/hub/systems/system_realtime.go:166`) |

The agent ignores decode errors and then uses zero options (`agent/handlers.go:103-104`). Our
agent must handle **any** CacheTimeMs value generically, keyed by its value, as upstream does.

### 4.2 Upstream caching and per-interval deltas

- **Response cache** (`agent/agent_cache.go:28-55`, used in `agent/agent.go:186-191`, `:261`):
  - Results are cached per CacheTimeMs.
  - A cached result is reused while it is younger than **CacheTimeMs/2**: 30 s for the minute
    interval, 500 ms for real time (`agent/agent_cache.go:39`).
  - A cache hit skips collection completely, so no deltas advance. It also returns the cached
    data **without Details**, even when IncludeDetails is set, because the details step is
    only on the fresh path (`agent/agent.go:187-191` vs `:263`). The hub simply asks again next
    time.
- **Details** (`agent/system.go:124-134`): attached only if CacheTimeMs == 60000 **and** (the
  hub asked, or details changed since they were last sent). So **real-time replies never carry
  Details.**
- **CPU** (`agent/cpu.go:11-23`, `:40-78`):
  - The previous CPU times are stored per CacheTimeMs.
  - At process start the 60000 baseline is taken (`init`, `:16-23`).
  - A new interval key is seeded from the 60000 baseline (`:51-54`). So the first answer for a
    new interval covers "since the 60000 baseline was last taken".
  - The baseline for the key advances on every fresh collection (`:76`).
  - If the total time delta is ≤ 0, everything is 0 (`:62-65`).
  - Per-core usage is the same, with its own map (`:87-112`).
- **Network** (`agent/network.go:144-244`):
  - Per CacheTimeMs, the agent keeps the summed byte totals and a timestamp (`netIoStats`), plus a
    per-NIC `DeltaTracker` (`agent/deltatracker/deltatracker.go:19-100`).
  - The first request for a key has 0 ms elapsed, so all rates are 0 (`:147-149`, `:181`,
    `:209`).
  - Rates are `delta_bytes * 1000 / ms_elapsed`, in **integer** arithmetic (`:189`, `:198`,
    `:210-211`).
  - A NIC counter that went down is treated as a reset: the delta is the new value
    (`:184-188`, `:193-197`).
  - If either total rate exceeds 10,000,000,000 B/s, all network state is reset and
    `Bandwidth = [0,0]` (`:225-238`).
- **Disk I/O** (`agent/disk.go:688-803`):
  - Previous counters are kept per CacheTimeMs and per device.
  - A new interval is seeded from `diskBaseline`, the latest counters seen by any interval
    (`:704-711`, `:805-812`).
  - Devices are skipped (they keep their last values) if less than 100 ms has elapsed
    (`:719-721`), or on the first sample of an interval if less than CacheTimeMs/2 has elapsed
    (`:725-727`).
  - If MiB/s exceeds 50,000, the baselines are reset (`:735-740`).
- **Memory, load, disk usage and uptime** are instantaneous, with no deltas.
- **Disk usage cache:** if `DISK_USAGE_CACHE` is set, the usage of non-root filesystems is
  refreshed only once per that duration (`agent/disk.go:630-685`).

Consequence for us: the real-time view (1000) must not move the 60000 baselines. Upstream keeps
them separate per key. DECISIONS.md/PLAN.md already require this.

### 4.3 CombinedData

`internal/entities/system/system.go:217-227`:

| Key | Field | Go type | Omit rule | Upstream on a bare host | vms-beszel |
|---|---|---|---|---|---|
| 0 | Stats | `Stats` | never | 4.4 | 4.4 |
| 1 | Info | `Info` | never | 4.5 | 4.5 |
| 2 | Containers | `[]*container.Stats` | **never** (nil is encoded as `null`) | `null` (Docker is unreachable, so `data.Containers` stays nil: `agent/agent.go:200-207`) | **`null` (0xF6)** |
| 3 | SystemdServices | `[]*systemd.Service` | omitempty | absent | absent |
| 4 | Details | `*Details` | omitempty | present only as described in 4.2 | the same |
| 5 | SystemdServicesUpdated | `bool` | omitempty | absent | absent |
| 6 | Monitors | `map[string]monitor.Result` | **never** | `{}` (0xA0): `GetResults` always makes a map (`agent/network_monitor.go:161-184`) | **`{}` (0xA0)** |

How the hub treats these:

- **Containers:**
  - Both nil and empty skip the container tables (`len(...) > 0`,
    `internal/hub/systems/system.go:276`).
  - For alerts, **nil** means "Docker state unknown": nothing triggers or resolves. **Empty** means
    "no unhealthy containers" and resolves an active ContainerHealth alert
    (`internal/alerts/alerts_container.go:60-70`, `:111-120`).
  - Send null.
- **Monitors:**
  - nil skips the monitor records (`internal/hub/systems/system.go:315-319`).
  - `{}` reaches `updateNetworkMonitorsRecords`, which returns at once for an empty map
    (`:441-444`), and also prunes the hub's probe cache (`:353-359`).
  - Both are harmless. Send `{}`, as upstream does.

### 4.4 Stats

`internal/entities/system/system.go:22-68`. Fields tagged `cbor:"-"` (MaxCpu, MaxMem,
MaxDiskReadPs, MaxDiskWritePs, MaxNetworkSent, MaxNetworkRecv, MaxBandwidth, MaxDiskIO,
MaxDiskIoStats) are **never sent**. The hub computes them during rollup. Keys 1, 14, 15, 18, 19,
23–25, 27 and 30 are unused.

| Key | Field (JSON) | Go type | Omit rule | Unit and how upstream fills it | vms-beszel |
|---|---|---|---|---|---|
| 0 | Cpu (`cpu`) | float64 | never | % busy, 0–100, 2dp. Busy = total − idle − iowait over the interval (`agent/system.go:161`, `agent/cpu.go:117-141`) | required |
| 2 | Mem (`m`) | float64 | never | total memory, GiB 2dp (`agent/system.go:216`) | required |
| 3 | MemUsed (`mu`) | float64 | never | used memory, GiB 2dp, ZFS ARC excluded (`agent/system.go:194-218`) | required |
| 4 | MemPct (`mp`) | float64 | never | used/total ×100, 2dp (`agent/system.go:211-219`) | required |
| 5 | MemBuffCache (`mb`) | float64 | never | cache+buffers−shared, GiB 2dp (`agent/system.go:217`, `:357-363`) | required (0 allowed) |
| 6 | MemZfsArc (`mz`) | float64 | omitempty | GiB 2dp | absent |
| 7 | Swap (`s`) | float64 | omitempty | swap total, GiB 2dp (`agent/system.go:196`) | page+swap file total |
| 8 | SwapUsed (`su`) | float64 | omitempty | GiB 2dp (`agent/system.go:197`) | as measured |
| 9 | DiskTotal (`d`) | float64 | never | root filesystem total, GiB 2dp (`agent/disk.go:672`, `:675`) | required |
| 10 | DiskUsed (`du`) | float64 | never | GiB 2dp (`agent/disk.go:673`, `:676`) | required |
| 11 | DiskPct (`dp`) | float64 | never | gopsutil `UsedPercent` of root, 2dp (`agent/disk.go:662`, `:677`). It is computed from the raw counters, not from the rounded GiB values. | required |
| 12 | DiskReadPs (`dr`) | float64 | omitzero | **deprecated**, still sent: root read MiB/s 2dp (`agent/disk.go:731`, `:788`) | send for parity (see 4.6) |
| 13 | DiskWritePs (`dw`) | float64 | omitzero | deprecated: write MiB/s 2dp (`:732`, `:789`) | as above |
| 16 | NetworkSent (`ns`) | float64 | omitzero | **deprecated, never set by the 0.21.0 agent** (only `agent/docker.go:372` sets the container field of the same name) | never send |
| 17 | NetworkRecv (`nr`) | float64 | omitzero | as 16 | never send |
| 20 | Temperatures (`t`) | map[string]float64 | omitempty | °C | absent (**never send an empty map**: see 4.8) |
| 21 | ExtraFs (`efs`) | map[string]*FsStats | omitempty | 4.7. Keyed by the custom name, else the device key. Only filesystems with DiskTotal > 0 (`agent/agent.go:236-258`). | as upstream |
| 22 | GPUData (`g`) | map[string]GPUData | omitempty | — | absent |
| 26 | Bandwidth (`b`) | [2]uint64 | omitzero | **[sent B/s, recv B/s]** over the selected NICs, integer (`agent/network.go:240`) | required |
| 28 | LoadAvg (`la`) | [3]float64 | **never** | 1/5/15-minute load average, 2dp (`agent/system.go:183-190`). Note that the JSON tag is `omitempty` but the CBOR tag has no omit rule. | required (derived on VMS) |
| 29 | Battery (`bat`) | Battery = [2]uint8 | omitzero | [percent, state] | absent |
| 31 | NetworkInterfaces (`ni`) | map[string][4]uint64 | omitempty | per NIC: **[up B/s, down B/s, total bytes sent, total bytes received]** (`agent/network.go:201`). Totals are the raw cumulative counters. | as upstream |
| 32 | DiskIO (`dio`) | [2]uint64 | omitzero | root **[read B/s, write B/s]**, integer (`agent/disk.go:729-730`, `:790-791`) | required if available |
| 33 | CpuBreakdown (`cpub`) | []float64 | omitempty | *0.21.1: 8 entries, see section 10.* 5 entries **[user, system, iowait, steal, idle]**, each a % of total time, clamped 0–100, 2dp (`agent/system.go:162-168`, `agent/cpu.go:67-74`). Nice, irq and softirq are in no entry; the UI shows them as "Other" = 100 − sum. | 5 entries |
| 34 | CpuCoresUsage (`cpus`) | Uint8Slice = []uint8 | omitempty | busy % per core, rounded to an integer 0–100 (`agent/cpu.go:103-108`). **CBOR byte string** (5.2). | as upstream |
| 35 | DiskIoStats (`dios`) | [6]float64 | omitzero | root **[read time %, write time %, utilisation %, r_await ms, w_await ms, weighted I/O % ×100]**, 2dp (`agent/disk.go:749-785`, `:794-799`) | only fill what VMS can measure; 4.8 |
| 36 | Fans (`f`) | map[string]uint16 | omitempty | RPM | absent |
| 37 | Batteries (`bats`) | map[string]uint8 | omitempty | % | absent |
| 38 | DiskIOTotal (`diot`) | [2]uint64 | omitzero | root cumulative [read bytes, write bytes] (`agent/disk.go:792-793`) | if available |
| 39 | ZfsPools (`z`) | map[string]*ZfsPool | omitempty | — | absent |
| 40 | WiFi (`wf`) | map[string]int8 | omitempty | dBm | absent |
| 41 | MemBytes (`mem`) | [4]uint64 | omitzero | *0.21.1:* **[total, used, buff/cache, ZFS ARC]** bytes (section 10) | sent; ARC 0 |
| 42 | SwapBytes (`swap`) | [2]uint64 | omitzero | *0.21.1:* **[total, used]** bytes (section 10) | sent |

### 4.5 Info

`internal/entities/system/system.go:167-198`:

| Key | Field (JSON) | Go type | Omit rule | Upstream meaning | vms-beszel |
|---|---|---|---|---|---|
| 0 | Hostname (`h`) | string | omitempty | deprecated, never set | **must be absent**: if non-empty with no Details, the hub moves it into Details (`internal/hub/systems/system.go:1096-1116`) |
| 1 | KernelVersion (`k`) | string | omitempty | deprecated, never set | absent |
| 2 | Cores (`c`) | int | omitzero | deprecated, never set | absent |
| 3 | Threads (`t`) | int | omitempty | logical CPU count (`agent/system.go:291`). The UI divides load by it (`SITE/components/systems-table/systems-table-columns.tsx:240`). | active CPUs |
| 4 | CpuModel (`m`) | string | omitempty | deprecated, never set | **must be absent**: if set, the UI never loads `system_details` (`SITE/components/routes/system/use-system-data.ts:130`) |
| 5 | Uptime (`u`) | uint64 | never | **seconds** since boot (`agent/uptime_linux.go:26-44`, `agent/system.go:289`) | from BOOTTIME |
| 6 | Cpu (`cpu`) | float64 | never | = Stats.Cpu (`agent/system.go:284`) | |
| 7 | MemPct (`mp`) | float64 | never | = Stats.MemPct (`:286`) | |
| 8 | DiskPct (`dp`) | float64 | never | = Stats.DiskPct (`:287`) | |
| 9 | Bandwidth (`b`) | float64 | **never** | deprecated MB/s, never set, so always **0.0** on the wire | send 0.0 |
| 10 | AgentVersion (`v`) | string | never | `"0.21.0"` (`agent/system.go:30`) | `"0.21.0"` |
| 11 | Podman (`p`) | bool | omitempty | deprecated | absent |
| 12 | GpuPct (`g`) | float64 | omitempty | — | absent |
| 13 | DashboardTemp (`dt`) | float64 | omitempty | °C | absent |
| 14 | Os (`os`) | uint8 | omitempty | deprecated (moved to Details) | absent (0) |
| 18 | BandwidthBytes (`bb`) | uint64 | never | Bandwidth[0]+Bandwidth[1], B/s (`agent/system.go:290`) | required |
| 19 | LoadAvg (`la`) | [3]float64 | never | = Stats.LoadAvg (`:285`) | |
| 20 | ConnectionType (`ct`) | uint8 | omitempty, omitzero | 2 = WebSocket, set on every reply (`agent/handlers.go:107-110`, enum at `internal/entities/system/system.go:158-164`) | **2** |
| 21 | ExtraFsPct (`efs`) | map[string]float64 | omitempty | per extra FS: `DiskUsed/DiskTotal*100`, 2dp, computed from the rounded GiB values (`agent/agent.go:237`, `:253-256`) | as upstream |
| 22 | Services (`sv`) | []uint16 | omitempty | — | absent |
| 23 | Battery (`bat`) | [2]uint8 | omitzero | — | absent |
| 24 | RootDiskName (`rdn`) | string | omitempty | custom root name from `FILESYSTEM=dev__name` (`agent/agent.go:239-243`) | as upstream |
| 25 | PackageUpdates (`pu`) | []uint16 | omitempty | — | absent |
| 26 | WiFi (`wf`) | map[string]WiFi | omitempty | — | absent |
| 27 | SystemdLogs (`jl`) | bool | omitempty | — | absent |

### 4.6 Details

`internal/entities/system/system.go:201-214`:

| Key | Field | Go type | Omit rule | Upstream | vms-beszel |
|---|---|---|---|---|---|
| 0 | Hostname | string | never | `os.Hostname()` (`agent/system.go:44`) | TCP/IP host name or SCSNODE |
| 1 | Kernel | string | omitempty | kernel version (`:78-81`). The UI shows it as the tooltip on the OS line when OsName is set (`SITE/components/routes/system/info-bar.tsx:69-75`). | e.g. the VMS version and build |
| 2 | Cores | int | never | physical cores (`agent/system.go:95-104`) | |
| 3 | Threads | int | never | logical CPUs (`:96-105`) | |
| 4 | CpuModel | string | never | (`:85-93`) | HW_NAME |
| 5 | Os | uint8 | never | 0 Linux, 1 Darwin, 2 Windows, 3 FreeBSD (`internal/entities/system/system.go:149-156`) | **0** |
| 6 | OsName | string | never | (`agent/system.go:68-77`) | `"OpenVMS <ver>"` |
| 7 | Arch | string | never | kernel arch, e.g. `x86_64` (`:45-49`) | ARCH_NAME |
| 8 | Podman | bool | omitempty | — | absent |
| 9 | MemoryTotal | uint64 | never | total memory in **bytes** (`:108-113`) | bytes |
| 10 | SmartInterval | time.Duration (int64 ns) | omitempty | only with `SMART_INTERVAL`. The hub then uses it instead of 1 h (`internal/hub/systems/system.go:191-197`). | absent (see Q10) |
| 11 | ZfsInterval | time.Duration | omitempty | `ZFS_INTERVAL` (`internal/hub/systems/system.go:198-203`) | absent |
| 12 | Sockets | int | omitempty | *0.21.1:* physical CPU packages, 0 if unknown (section 10) | absent |

The hub upserts `system_details` from these fields (`internal/hub/systems/system.go:368-391`). The
UI never uses Os outside 0–3. **Any other value crashes the system page**: `osInfo[os]` is
undefined and is then destructured (`SITE/components/routes/system/info-bar.tsx:64-102`). With
Os = 0 the UI shows the Tux icon and the text `OsName || Kernel`.

### 4.7 FsStats (an entry of Stats key 21)

`internal/entities/system/system.go:121-140`. Root, Mountpoint and Name are `json:"-"` and have
no CBOR tag. They are internal only and **must not be sent**; under fxamacker's default they
would otherwise be encoded under their Go field names (Q6).

| Key | Field (JSON) | Go type | Omit rule | Unit |
|---|---|---|---|---|
| 0 | DiskTotal (`d`) | float64 | never | GiB 2dp (`agent/disk.go:672`) |
| 1 | DiskUsed (`du`) | float64 | never | GiB 2dp (`:673`) |
| 2 | DiskReadPs (`r`) | float64 | **never**; *omitzero in 0.21.1* | deprecated: read MiB/s 2dp (`:776`); the 0.21.1 agent no longer sets it |
| 3 | DiskWritePs (`w`) | float64 | **never**; *omitzero in 0.21.1* | deprecated: write MiB/s 2dp (`:777`); as key 2 |
| 6 | DiskReadBytes (`rb`) | uint64 | omitempty | read B/s (`:778`) |
| 7 | DiskWriteBytes (`wb`) | uint64 | omitempty | write B/s (`:779`) |
| 8 | DiskIoStats (`dios`) | [6]float64 | omitzero | as Stats key 35 (`:780-785`) |
| 9 | TotalRead (`tr`) | uint64 | omitzero | cumulative read bytes (`:774`) |
| 10 | TotalWrite (`tw`) | uint64 | omitzero | cumulative write bytes (`:775`) |

**Never send an ExtraFs entry with DiskTotal = 0.** The hub's live Disk alert computes
`DiskUsed/DiskTotal*100` with no zero check, so `d = 0, du > 0` gives +Inf and fires the alert
(`internal/alerts/alerts_system.go:72-79`). Upstream avoids this by including only entries with
DiskTotal > 0 (`agent/agent.go:245`).

About the deprecated rates (Stats `dr`/`dw`, FsStats `r`/`w`): the hub moves `dr/dw × 1 MiB`
into `dio` **only when `dio` is [0,0]**, and then zeroes `dr/dw`
(`internal/hub/systems/system.go:1090-1094`). The UI falls back from `dio`/`rb`/`wb` to
`dr`/`dw`/`r`/`w` (`SITE/components/routes/system/charts/disk-charts.tsx:38-58`). The real-time
path does **not** run that migration (`internal/hub/systems/system_realtime.go:166-173`). Sending
both, consistently, is what upstream does.

### 4.8 What the hub and UI read

**"Up" status.** `update()` → `createRecords` → the systems record is saved with
`status = "up"` (`internal/hub/systems/system.go:162-235`, `:326`). The only requirements are:

- the reply arrives within 30 s;
- it decodes into AgentResponse and then CombinedData with no type error (section 5);
- it has no Error.

The values themselves are not checked. Writes use `SaveNoValidate` and raw SQL, so schema limits
do not apply.

The stored `systems.info` is the whole Info struct. Info `g` is added only if Stats `g` is
non-empty (`internal/hub/systems/system.go:327-335`).

**Rollups.** These are hub-internal, using `internal/records/records.go` (the 1m → 10m → 20m →
120m → 480m tiers, `:44-70`):

- Most float fields and the arrays `b`, `dio`, `dios`, `la` are averaged.
- Max fields are derived.
- `ni[*][2..3]` and `diot`, `tr`, `tw` are max'd.
- Nothing can divide by zero because of our data.

**Alerts** (`internal/alerts/alerts_system.go`). Live value from Info or Stats; the averaged value
comes from the 1m `system_stats` records.

| Alert | Live value | Averaged value |
|---|---|---|
| CPU | Info.cpu (`:65-66`) | stats.cpu (`:234`) |
| Memory | Info.mp (`:67-68`) | stats.mp |
| Bandwidth | Info.bb / 1 MiB, labelled "MB/s" (`:69-71`) | (b[0]+b[1]) / 1 MiB (`:238`) |
| Disk | max of Info.dp and each efs du/d×100 (`:72-88`) | per filesystem (`:239-266`) |
| Temperature | **skipped if Info.dt < 1** (`:89-94`) | — |
| LoadAvg1/5/15 | Info.la[0..2] (`:95-103`) | stats.la |
| GPU | Info.g (`:104-105`) | — |
| Battery | Stats.bat; skipped with no battery (`:106-110`) | — |
| CPUIOWait / CPUSteal | Stats.cpub[2] / [3]; needs ≥ 5 entries summing to > 0 (`:16-37`, `:111-115`) | — |

- Status ("system down") alerts follow up/down transitions (`internal/hub/systems/system_manager.go:264-267`,
  `internal/alerts/alerts_status.go`).
- All the alerts that are meaningful on VMS work from the fields above: CPU, Memory, Disk,
  Bandwidth, LoadAvg, CPUIOWait and CPUSteal (the last two are always 0), and Status.

**Charts** (UI, `SITE/components/routes/system/`):

| Chart | Data | Shown when |
|---|---|---|
| CPU | `cpu` | always |
| CPU breakdown and per-core sheet | `cpub`, `cpus` | needs v ≥ 0.15.3, plus a non-empty `cpub` or `cpus` in the last record (`cpu-sheet.tsx:13`, `:29-43`) |
| Memory | `mu`, `mb`, domain `m` | always (`charts/memory-charts.tsx`) |
| Swap | `su` | **only if the last `su` > 0** (`charts/memory-charts.tsx:139`). `s` is only the axis maximum. |
| Disk usage | `du`/`d`, title from Info `rdn` | always (`charts/disk-charts.tsx:115-156`) |
| Disk I/O | `dio`, else `dr`/`dw` × 1 MiB | always (`charts/disk-charts.tsx:158-222`) |
| "More I/O" sheet (every disk chart, including extra filesystems) | — | the button appears only if some record has **root** `dios[0]` truthy (`charts/disk-charts.tsx:172-182`) |
| Inside the I/O sheet: utilisation, queue depth, await charts | root `dios[2]`, `dios[5]`, `dios[3]` | each only if that value is > 0 (`disk-io-sheet.tsx:81-92`) |
| Inside the I/O sheet: cumulative charts | `diot` (`tr`/`tw` for extra filesystems) | value > 0 |
| Extra filesystems | `efs` | the last `efs` is non-empty (`charts/disk-charts.tsx:268-295`) |
| Bandwidth | `b`, else `ns`/`nr` × 1 MiB | always (`charts/network-charts.tsx:14-88`) |
| Per-interface sheet | `ni` | button hidden if the last `ni` is empty (`network-sheet.tsx:37-39`) |
| Load average | `la` | hidden only for agent 0.≤12 (`charts/load-average-chart.tsx:16-19`) |
| Temperatures, fans | `t`, `f` | shown whenever the map is present, **even if empty** (`charts/sensor-charts.tsx:111`, `:256`), so never send empty maps |
| 1-minute real-time chart | Stats and `container` from the raw CombinedData JSON | needs v ≥ 0.13 and status up (`use-system-data.ts:143-184`). Info and Details are not read. |

### 4.9 Real time and deprecated-field migration

`migrateDeprecatedFields` runs on every 60 s update (`internal/hub/systems/system.go:1077-1117`):

- If `b` is [0,0], it sets `b` from `ns`/`nr`.
- If `bb` is 0, it sets `bb` from Info `b`.
- If `dio` is [0,0], it sets `dio` from `dr`/`dw`.
- If there are no Details and Info `h` is non-empty, it builds Details from Info.

None of this runs for real time (`internal/hub/systems/system_realtime.go:166-173`). Send the
current fields directly.

---

## 5. CBOR encoding rules

### 5.1 Options in use

- Nobody sets `EncOptions` or `DecOptions`. A search for `EncMode`, `DecMode`, `EncOptions` and
  `DecOptions` finds nothing.
- Every call is plain `cbor.Marshal` or `cbor.Unmarshal` with the fxamacker defaults, for
  example:
  - `internal/hub/ws/request_manager.go:94`, `:105`
  - `internal/hub/transport/websocket.go:49`
  - `internal/hub/transport/transport.go:38`
  - `agent/client.go:261`, `:380`
  - `agent/response.go:31`
- Everything in 5.2–5.4 below therefore depends on the **fxamacker v2.9.4 defaults [lib]**. The
  conformance suite (D6) must confirm each point by decoding our bytes with upstream's own
  structs.

### 5.2 Type mapping (what upstream emits, and what we must emit)

| Go type | CBOR emitted by upstream [lib] | Rule for vms-beszel |
|---|---|---|
| `float64` | always major 7, **8-byte double** (0xFB). The default ShortestFloat is "none". | always 0xFB + IEEE big-endian. Do not shorten. |
| `uint8`/`uint16`/`uint32`/`uint64`, `int` | shortest integer head (major 0, or major 1 for negatives) | shortest form. The hub decodes any width. Negative into an unsigned field is an error. |
| `time.Duration` | int64 nanoseconds, as an integer | (not sent) |
| `bool` | 0xF4 / 0xF5 | |
| `string` | major 3, text | must be **valid UTF-8**: the default decoder rejects invalid UTF-8 [lib]. Clean VMS strings (DEC MCS bytes ≥ 0x80) before sending. |
| `[]byte` (FingerprintRequest.Signature) | major 2, byte string | decode as a byte string |
| `Uint8Slice` (`[]uint8`, Stats `cpus`) | **major 2, byte string** (`internal/entities/system/system.go:83-86`: "CBOR: falls back to default handling for []uint8 (byte string)") | **byte string**, one byte per core |
| `Battery [2]uint8` | byte array → byte string, by the default ByteArray option [lib] | not sent |
| `[N]uint64`, `[N]float64` (`b`, `dio`, `diot`, `la`, `dios`, `ni` values) | major 4 array of N items | fixed length N. Never send a shorter array. |
| `[]float64` (`cpub`), `[]uint16` | major 4 array | |
| `map[string]X` | major 5, text-string keys | text keys |
| struct with `keyasint` | major 5 map with unsigned integer keys | integer keys |
| nil pointer or slice without omit (`Containers`) | `null` 0xF6 (NilContainers default) [lib] | 0xF6 |
| `cbor.RawMessage` (`Data`) | the embedded item, verbatim | embed the encoded item; do **not** wrap it in a byte string |

### 5.3 Omit rules

These are fxamacker semantics [lib], matching encoding/json Go 1.24.

- **omitempty:**
  - number 0, `false`, `""`;
  - slice or map with **length 0 (nil or empty)**;
  - nil pointer or interface;
  - an array only if its length is 0, so `[3]float64` with omitempty is never omitted;
  - a struct only if it has no fields that are not omitempty and all its fields are empty.
- **omitzero:** the Go zero value. For an array, all elements are zero; for a float, only +0.0
  counts (−0.0 is not zero).

The tables in 3 and 4 give each field's rule. In practice:

- send keys tagged "never" every time;
- leave out omitempty/omitzero keys when their value is zero or empty.

The hub does not need the omissions: a zero value decodes the same as an absent key. They only
keep frames identical to upstream's for the golden-fixture comparison.

### 5.4 Decoder behaviour on the hub [lib]

- **Key order:** with the default (SortNone), upstream writes struct fields in declaration order
  and maps in Go's random order. The hub accepts any order. For fixtures, emit struct keys in
  declaration order. Never repeat a key.
- **Unknown keys** are **ignored**: the default ExtraReturnErrors is none. Upstream's integer keys
  are all < 24, so we can use one-byte heads.
- **Wrong types fail.** For example: a text string where a number is expected, a float into an
  integer field, a negative number into an unsigned field, an array of the wrong length into a
  Go array (believed to be an error), or a map key that is not text in `map[string]X`.
  - Decoding into a struct continues after the first type error but **returns** that error. The
    whole GetData reply is then rejected, and the hub **closes the WebSocket** (section 3.3).
  - **An integer where float64 is expected:** believed to be accepted (converted). We still always
    send 0xFB floats, to be safe and byte-compatible (Q4).
- **Limits:** default nesting, array and map sizes (believed: depth 32, 131072 elements) are far
  above what we send.
- **Indefinite-length items:** accepted by default. We emit only definite lengths.

### 5.5 Example frames

Ids are 1-based. Bytes are in hex.

- **Fingerprint reply to Id 1**, fingerprint `abc…` (48 characters):
  `A2 00 01 02 A1 00 78 30 <48 ASCII bytes>`, meaning `{0: 1, 2: {0: "…"}}`.
- **Monitor sync reply to Id 2:** `A2 00 02 07 A0`.
- **Unsupported reply to Id 9:** `A2 00 09 03 75 "unsupported operation"` (21 bytes).

---

## 6. Agent behaviour to mirror

### 6.1 Connection manager

`agent/connection_manager.go`:

- **Start** (`:163-220`):
  - Build the client. A CA file error is fatal (`:168-173`).
  - Start the 10 s ticker (`:58`, `:193`).
  - Connect at once (`:194`).
  - Then loop over: events, ticker ticks, the health tick every 90 s, and SIGINT/SIGTERM.
- **Connection attempts** (`startWebSocketConnection`, `:358-375`):
  - Only while Disconnected.
  - At most one attempt per 5 s (`:365-367`).
  - On failure, log `WebSocket connection failed` (`:370-371`).
- **Retry pacing:**
  - The ticker fires every **10 s** while not connected (`:206-212`). There is **no
    exponential backoff**.
  - After a drop from the connected state, the agent reconnects at once, or after 5 s if the
    previous attempt was less than 5 s ago (`:335-341`).
  - The ticker is re-armed so that retries continue (`:309-330`).
- **"Connected"** is reached only when the hub has been **verified**, i.e. the signature
  checked out (`agent/client.go:297`, `agent/connection_manager.go:260-266`). That stops the ticker
  (`:301-304`).
  - A connection that upgrades but then fails authentication closes, and the ticker retries
    every 10 s (`:271-281`, `:318-320`).
- **Disconnect** (`OnClose`, `agent/client.go:231-244`): clear the connection and the verified
  flag, log `Connection closed` with the error, and post WebSocketDisconnect.
  - The handler's `Disconnected` path logs `Disconnected from hub` (`:325`) and reconnects.
- **Health file** (`agent/health/health.go`): `beszel_health` is touched at start and every
  90 s (`agent/connection_manager.go:196-198`). The `health` subcommand fails if the file is
  older than 91 s. This is local only, and optional for us.
- **`EXIT_ON_DNS_ERROR=true`:** on a DNS lookup failure the agent sleeps 2 s and exits 1
  (`:345-350`, `:412-419`).
- **SSH fallback:** upstream starts its SSH server whenever it is Disconnected (`:309-313`,
  `:377-401`). We do not.

### 6.2 Per-message behaviour

- **Every hub message**, of any type, resets the 120 s deadline (`agent/client.go:253`).
- **Text frames** are ignored (`:255-257`).
- **A message that is not valid CBOR for HubRequest** is logged as `Error parsing message`
  and ignored (`:259-265`).
- **Pings** get a pong with the same payload (`:274-277`).
- **Write failures** close the connection with 1000, which triggers a reconnect
  (`:387-392`).

### 6.3 On reconnect

The hub replaces the system object (`internal/hub/systems/system_manager.go:348-368`). It then:

- requests details again (`IncludeDetails` on the first update);
- clears failed SMART/ZFS cooldowns, so it sends GetSmartData and GetZfsData again
  (`:353`, `:370-396`);
- sends a fresh monitor replace-sync.

Our per-interval baselines can survive a reconnect. Upstream keeps them across connections,
because they belong to the Agent, not the connection.

---

## 7. Configuration variables

**Lookup.** `utils.GetEnv(key)` first tries `BESZEL_AGENT_<key>`, then `<key>`. "Set" means
present, even if empty (`agent/utils/utils.go:17-22`). DECISIONS.md D5 maps this onto the config
file plus `BESZEL$<key>` logical names.

| Variable | Parsing in 0.21.0 | Relevance for vms-beszel |
|---|---|---|
| `HUB_URL` | `url.Parse`; must have a host (`agent/client.go:72-82`). Scheme mapping in 1.1. Overridden by `-url` (`internal/cmd/agent/agent.go:111-113`). If unset, upstream runs in SSH-only mode (`agent/client.go:35-38`). | required |
| `TOKEN` | used if non-empty (`agent/client.go:105-108`). Overridden by `-token`. | one of TOKEN / TOKEN_FILE |
| `TOKEN_FILE` | read if TOKEN is empty (`:110-118`). Each line is trimmed; blank and `#` lines are skipped; **more than one token is an error**; an empty file gives `""` (`:124-139`). | |
| `KEY` | used if set **and** non-empty (`internal/cmd/agent/agent.go:128-130`). Format in 2.3. | one of KEY / KEY_FILE |
| `KEY_FILE` | read if KEY is unset or empty. If the variable is not set at all, the agent exits with "no key provided" (`:133-142`). | |
| `CA_CERT_FILE` | PEM file. Its certificates are **added** to the system roots; a read or parse error is **fatal** at start (`agent/client.go:144-164`, `agent/connection_manager.go:168-173`). Normal hostname verification applies. | SSL3: add to the trust store |
| `HUB_HEADERS` | **does not exist in 0.21.0**; 0.21.1 adds it (section 10) | not supported (D5, D16) |
| `SYSTEM_NAME` | only sent as `Name` in the universal-token reply (`agent/client.go:304`) | |
| `LISTEN` / `PORT` | SSH listen address. LISTEN, else PORT, else `:45876`; a bare port gets a `:` prefix (`agent/server.go:256-272`). In WS mode it only supplies `Port` in the universal-token reply (`agent/client.go:306-307`). | optional; default `"45876"` |
| `NETWORK` | SSH network type `tcp`/`unix` (`agent/server.go:277-285`) | not applicable |
| `DATA_DIR` | tried first, then `/var/lib/beszel-agent`, then `~/.config/beszel`. The first that exists and is writable wins, else the first that can be created (`agent/data_dir.go:16-68`). If none works, the agent carries on without one, and the fingerprint is not saved (`agent/agent.go:74-79`). | default `BESZEL$ROOT:[DATA]` |
| `FILESYSTEM` | `device[__name]`, split at the first `__`, both parts trimmed (`agent/disk.go:69-78`, `:338-339`). It picks the root filesystem by mountpoint, device name or a prefix relation (`:198-219`, `:541-559`). `name` becomes Info `rdn`. | the VMS device for the root, e.g. `DKA0:__System` |
| `EXTRA_FILESYSTEMS` | comma-separated `fs[__name]` entries (`agent/disk.go:296-301`). Each matches a partition by device suffix or mountpoint, else any path that statfs accepts (`:269-292`). The Stats `efs` key is `name`, else the device key (`agent/agent.go:246-251`). Extra filesystems with the same usage as root (±16 MiB) are dropped (`agent/disk.go:415-462`). | comma list of VMS devices, with optional `__name` |
| `NICS` | comma list, trimmed, empty entries ignored. A leading `-` makes it a blacklist. `*` wildcards use `path.Match` (`agent/network.go:29-77`). Whitelist mode skips the automatic filter. Blacklist mode or unset applies it: names starting `lo`, `docker`, `br-`, `veth`, `bond`, `cali`, and NICs with 0 bytes sent or received, are dropped (`:247-272`). Whitelist with an empty list selects nothing (`:51-53`). | the same syntax over VMS LAN device names (e.g. `EWA0`) |
| `LOG_LEVEL` | case-insensitive `debug`, `warn` or `error`; anything else means info (`agent/agent.go:95-105`) | |
| `MEM_CALC` | only `htop` has an effect: used = total − free − cache/buffers (`agent/agent.go:81`, `agent/system.go:364-366`) | decide in D9 |
| `DISK_USAGE_CACHE` | Go duration (`15m`, `1h`). An invalid value is logged and ignored (`agent/agent.go:85-92`). It caches the usage of non-root filesystems only. | optional |
| `DISABLE_SSH` | agent: never start the SSH server (`agent/connection_manager.go:384-387`) | implicitly true |
| `EXIT_ON_DNS_ERROR` | `true` → exit 1 on a DNS failure (6.1) | optional |
| `BESZEL_AGENT_ALL_PROXY` | SOCKS proxy (1.2) | out of scope for v1 |

Not relevant to VMS: `DOCKER_HOST`, `SENSORS`, `SYS_SENSORS`, `PRIMARY_SENSOR`, `SKIP_GPU`,
`SMART_*`, `ZFS_INTERVAL`, `SKIP_SYSTEMD`, `SERVICE_PATTERNS`, `PACKAGE_UPDATES_INTERVAL`,
`SKIP_WIFI` and the rest of the list from `grep GetEnv agent/`.

---

## 8. Open questions

Each question says what would settle it.

- **Q1. gws defaults (hub `ServerOption{}`, agent `ClientOption`).** Open points:
  - the maximum read payload (believed 16 MiB);
  - the handshake timeout;
  - whether the server rejects a client that offers permessage-deflate, or just ignores it;
  - whether unmasked client frames are rejected;
  - how a received close frame is echoed;
  - whether `SetDeadline` covers reads and writes.

  *Settle:* read gws v1.10.2 (`option.go`, `upgrader.go`, `conn.go`), or run the Linux
  integration hub with crafted frames.
- **Q2. Ed25519 `Blob` layout.** **Settled (section 9):** the raw 64-byte signature over the
  raw token bytes. It verifies with libcrypto's plain Ed25519 using the 32-byte key from the
  `ssh-ed25519` line.
- **Q3. blang/semver strictness** for `X-Beszel`. *Settle:* blang v3.5.1 `semver.go`, or a test
  against the integration hub with `v0.21.0` and `0.21`.
- **Q4. fxamacker decoder conversions.** **Moot (Phase 1):** the conformance tests show we
  send exactly the bytes upstream's encoder produces for the same values, so the hub's
  decoder never has to convert anything (`tests/conformance`).
- **Q5. Text keys for ZfsData.** **Settled (Phase 1):** fxamacker uses the `json` tag
  name. `cbor.Marshal(zfs.ZfsData{Complete: true})` is `{"complete": true}`, byte for byte
  our reply (`tests/conformance`, TestReplies).
- **Q6. Exact upstream bytes for the empty SyncResponse.** **Settled (section 9):** upstream
  sends `{0:{8:0,10:0,11:0,12:0}}`. fxamacker's `omitempty` does not omit a struct-typed field,
  so the zero `Result` is encoded with its four non-omitempty fields. We send the same bytes
  (3.3, action 7). Whether untagged fields such as FsStats Root/Mountpoint/Name are encoded is
  still open; the conformance tests settle it.
- **Q7. gopsutil `UsedPercent`** for DiskPct: used/(used+avail), which differs from
  used/total. We need the exact VMS analogue for D9. *Settle:* the gopsutil v4.26.9 source.
  This is our own decision anyway.
- **Q8. PocketBase middleware** in front of `/api/beszel/agent-connect`: rate limits, body or
  header size limits, behaviour behind a proxy. *Settle:* PocketBase v0.40.4 defaults, plus an
  integration test behind a reverse proxy.
- **Q9. Network monitors.** Users can create monitors for our system, because v ≥ 0.20.0
  (`SITE/lib/utils.ts:418-422`). We accept the sync and never report results, so those monitors
  show no data. Alternatives:
  - implement probes (on the "later" list in PLAN.md);
  - accept that the monitors page offers our system.

  *Settle:* a decision by the user.
- **Q10. SMART hourly Info logs on the hub.** These are unavoidable for any agent that reports
  ≥ 0.15 and has details. Should we instead send a large `SmartInterval` in Details (key 10) to
  space them out, for example 24 h? That changes nothing visible but differs from upstream on
  a bare host. *Settle:* a decision by the user.
- **Q11. The `Kernel` tooltip and `Arch` text** on VMS. What exactly should be shown? A cosmetic
  D9 item.
- **Q12. HUB_HEADERS** is in D5 but not in upstream 0.21.0. Keep it as an extension, or drop it?
  *Settled at the Phase 0 exit:* dropped. Upstream 0.21.1 added it (section 10); whether to
  support it now is open (D16).

## 9. Evidence from the reference capture (Phase 0)

The upstream release binaries (`beszel` and `beszel-agent` 0.21.0 for linux/amd64, checked
against `beszel_0.21.0_checksums.txt`) ran on Linux with `tools/wscap.py` between them.
`tools/wsdecode.py` decoded the traffic. The session lasted 200 s, with a 20 s real-time
subscription (`tools/rtsub.py`) at t=80 s. The captures and frames are in git-ignored
`out/capture/`: they hold the workstation's real data, so they are not committed.

| t (s) | Direction | Frame (CBOR, Id-keyed) | Confirms |
|---|---|---|---|
| 0 | a2h | upgrade: `X-Beszel: 0.21.0`, `X-Token`, Chrome User-Agent, no `Sec-WebSocket-Extensions`, no subprotocol | 1.2 |
| 0 | h2a | `101`, no extensions accepted | Q1 (deflate not in use) |
| 0 | h2a | `{0:1, 1:{0:h'<64 bytes>', 1:false}, 2:1}` | 2.1; Q2 |
| 0 | a2h | `{0:1, 2:{0:"<48 hex>"}}` (legacy key 2) | 2.4 |
| 0 | h2a | `{0:0, 1:{0:60000, 1:true}, 2:2}` | 4.1 |
| 0 | h2a | `{0:7, 1:{0:0, 1:{0:"",1:"",2:"",4:0}}, 2:3}` (replace sync) | 3.2, action 7 |
| 0 | a2h | `{0:2, 1:CombinedData}`: 5368 bytes; floats are all `0xFB`; `cpus` and `bat` are byte strings; key 2 (Containers) is `null` | 4.3, 5.2 |
| 0 | a2h | `{0:3, 7:{0:{8:0,10:0,11:0,12:0}}}` | Q6 |
| 54 | h2a | `{0:0, 1:{0:60000, 1:true}, 2:4}`: the jittered first tick, details still requested | 1.5 |
| 54 | h2a | `{0:4, 2:5}`: GetSmartData, no Data | 3.2, action 4 |
| 54 | a2h | `{0:5}`: upstream on a host without smartctl | 3.3, action 4 |
| 81–101 | both | `{0:0, 1:{0:1000, 1:false}, 2:n}` once a second, replies of 695–786 bytes | 4.1, 4.9 |
| 114, 174 | both | 60 s ticks: `{0:60000, 1:false}` | 1.5 |

The first frame's signature verified (on Linux, `openssl pkeyutl -verify -rawin`) as Ed25519
over the token bytes, with the key from the hub's `ssh-ed25519` line. The same check with a
changed token failed. No ZFS request was seen in 200 s.

## 10. Changes in 0.21.1

Upstream released 0.21.1 on 2026-10-22. References in this section are to
`cache/beszel-0.21.1`. Found by comparing the two tags (`gh api
repos/henrygd/beszel/compare/v0.21.0...v0.21.1`) and confirmed by the conformance tests (D6)
and `tests/integration/itest.py` against the 0.21.1 release hub.

**Unchanged:** the URL, the upgrade headers the hub checks, the handshake, the envelope keys
(`internal/common/common-ws.go:37-55`; only legacy SmartData key 5 lost `omitempty`, which
changes nothing for an absent map), the action numbers and the GetData request.
`X-Beszel` and `Info.AgentVersion` are now `0.21.1` (`beszel.go:9`).

**Memory and swap in bytes.** Two new Stats fields (`internal/entities/system/system.go:62-64`):

| Key | Field (JSON) | Go type | Omit rule | Content |
|---|---|---|---|---|
| 41 | MemBytes (`mem`) | [4]uint64 | omitzero | [total, used, buff/cache, ZFS ARC] bytes |
| 42 | SwapBytes (`swap`) | [2]uint64 | omitzero | [total, used] bytes; used = total − free, never below 0 |

The 0.21.1 agent fills both and still fills the GB fields (keys 2, 3, 5-8) from the same
values, for hubs older than 0.21.1 (`agent/memory.go:18-55`). The GB fields are marked
deprecated and will be removed upstream in a later release. MemPct is used/total from the
bytes, as before. The hub derives the bytes from the GB fields **only when MemBytes or
SwapBytes is all zero** (`internal/hub/systems/system.go:1125-1137`); otherwise it stores
what the agent sent. The UI reads `mem`/`swap` and falls back to the GB fields for older
records (`SITE/types.d.ts:108-127`).

**FsStats rates omitted when zero.** FsStats DiskReadPs/DiskWritePs (keys 2, 3) changed from
"never omitted" to omitzero (`internal/entities/system/system.go:126-127`); the 0.21.1 agent
no longer sets them, and the hub folds any it gets into `rb`/`wb`
(`internal/hub/systems/system.go:1114-1124`). The UI reads only `rb`/`wb`, defaulting to 0
(`SITE/components/routes/system/charts/disk-charts.tsx:46-58`). Stats keys 12/13 were already
omitzero.

**CpuBreakdown has 8 entries:** [user, system, iowait, steal, idle, **irq, softirq, nice**]
(`internal/entities/system/system.go:53`, `agent/system.go:166-177`). The UI charts entries 5-7
when present and "Other" as 100 − sum (`SITE/components/routes/system/cpu-sheet.tsx:58-123`),
so a 5-entry breakdown still displays.

**Details.Sockets** (key 12, int, omitempty): physical CPU packages, 0 if unknown
(`internal/entities/system/system.go:206`); the hub stores it in `system_details.sockets`
(`internal/hub/systems/system.go:384`).

**HUB_HEADERS** (agent): comma- or newline-separated `Name: value` pairs added to the upgrade
request; X-Token and X-Beszel in it are ignored with a warning, and values are never logged
(`agent/client.go:176-206`, `:241-248`).

**Hub timing.** The hub's read deadline is now 100 s, not 70 s (`internal/hub/ws/ws.go:26`).
When a poll fails, the hub pings the agent to keep the connection, unless the agent has sent
nothing for 3 minutes (`internal/hub/systems/system.go:177-183`, `:820`). Both only make the
hub more tolerant of a slow agent.

