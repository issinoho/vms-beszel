#!/usr/bin/env bash
# vmsrun.sh - run the built agent on a node against a hub for a while.
#   vmsrun.sh <node> <system.env> <hub-url> <seconds> [extra config lines...]
# <system.env> is what `refhub.sh init` printed (SYSTEM_ID, KEY, TOKEN).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
node=$1 envf=$2 url=$3 secs=$4; shift 4
. "$envf"
read -r _ _ _ _ _ WORKDIR _ < <(awk -v n="$node" '$1==n' "$root/tools/nodes.conf")
data="${WORKDIR%]}.RUN.DATA]"
conf=$(mktemp)
{ echo "# written by tools/vmsrun.sh"
  echo "HUB_URL=$url"; echo "TOKEN=$TOKEN"; echo "KEY=\"$KEY\""; echo "DATA_DIR=$data"
  echo "LOG_LEVEL=debug"; for l in "$@"; do echo "$l"; done; } > "$conf"
"$root/tools/vms.sh" "$node" put "$conf" -- run >/dev/null 2>&1 || true
"$root/tools/vms.sh" "$node" dcl "rename [.run]$(basename "$conf") [.run]agent.conf" "purge/keep=1 [.run]agent.conf" \
    "set file/protection=(s:rwed,o:rwed,g,w) [.run]agent.conf" >/dev/null   # it holds the token
rm -f "$conf"
VMS_BATCH_POLL_SECS=15 "$root/tools/vms.sh" "$node" batch "$root/probes/run_agent.com" "$secs"
