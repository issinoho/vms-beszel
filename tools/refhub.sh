#!/usr/bin/env bash
# refhub.sh - a private reference beszel hub (upstream release binary) on Linux.
#
#   refhub.sh start           start the hub on 127.0.0.1:$REFHUB_PORT (default 18090)
#   refhub.sh init <name>     first user, then a system <name> with a token, as the
#                             UI's "add system" does; prints KEY= and TOKEN= lines
#   refhub.sh api <path>      GET an authenticated hub API path (JSON)
#   refhub.sh stop
#
# Everything lives under out/refhub; the hub binary is cache/ref (tools/fetch.sh --ref).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
port=${REFHUB_PORT:-18090}
base=http://127.0.0.1:$port
dir=$root/out/refhub
hub=$root/cache/ref/beszel_linux_amd64/beszel
email=ref@example.invalid
pw=refhub-$(hostname | sha256sum | cut -c1-16)   # local throwaway hub only
mkdir -p "$dir"
auth() {
    curl -sf "$base/api/collections/users/auth-with-password" -H 'Content-Type: application/json' \
        -d "{\"identity\":\"$email\",\"password\":\"$pw\"}" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["token"])'
}
case ${1:-} in
start)
    [ -f "$dir/hub.pid" ] && kill -0 "$(cat "$dir/hub.pid")" 2>/dev/null && { echo "running"; exit 0; }
    setsid nohup "$hub" serve --http "127.0.0.1:$port" --dir "$dir/data" >"$dir/hub.log" 2>&1 &
    echo $! >"$dir/hub.pid"
    for _ in $(seq 50); do curl -sf "$base/api/health" >/dev/null && { echo "hub up on $base"; exit 0; }; sleep 0.2; done
    echo "hub did not start; see $dir/hub.log" >&2; exit 1 ;;
init)
    name=${2:?system name}
    curl -sf "$base/api/beszel/create-user" -H 'Content-Type: application/json' \
        -d "{\"email\":\"$email\",\"password\":\"$pw\"}" >/dev/null 2>&1 || true
    tok=$(auth)
    uid=$(curl -sf "$base/api/collections/users/records" -H "Authorization: $tok" |
        python3 -I -c 'import json,sys; print(json.load(sys.stdin)["items"][0]["id"])')
    sys=$(curl -sf "$base/api/collections/systems/records" -H "Authorization: $tok" -H 'Content-Type: application/json' \
        -d "{\"name\":\"$name\",\"host\":\"$name\",\"port\":\"45876\",\"users\":\"$uid\"}" |
        python3 -I -c 'import json,sys; print(json.load(sys.stdin)["id"])')
    token=$(python3 -I -c 'import uuid; print(uuid.uuid4())')
    curl -sf "$base/api/collections/fingerprints/records" -H "Authorization: $tok" -H 'Content-Type: application/json' \
        -d "{\"system\":\"$sys\",\"token\":\"$token\"}" >/dev/null
    key=$(curl -sf "$base/api/beszel/info" -H "Authorization: $tok" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["key"])')
    echo "SYSTEM_ID=$sys"; echo "KEY='$key'"; echo "TOKEN=$token" ;;
api)
    curl -sf "$base${2:?path}" -H "Authorization: $(auth)" ;;
stop)
    [ -f "$dir/hub.pid" ] && kill "$(cat "$dir/hub.pid")" 2>/dev/null; rm -f "$dir/hub.pid"; echo stopped ;;
*) sed -n '2,10p' "$0" >&2; exit 2 ;;
esac
