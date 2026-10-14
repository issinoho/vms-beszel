#!/usr/bin/env bash
# vmstree.sh - lay out a BESZEL$ROOT test tree, [.ROOT] in a node's work directory,
# as the kit will: [BIN] the built image, [COM] the procedures, [CONF]AGENT.CONF
# (owner-only: it holds the token), [DATA], [LOG].
#   vmstree.sh <node> <system.env> <hub-url> [extra config lines...]
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
v=$root/tools/vms.sh
node=$1 envf=$2 url=$3; shift 3
. "$envf"
conf=$(mktemp)
{ echo "# written by tools/vmstree.sh"
  echo "HUB_URL=$url"; echo "TOKEN=$TOKEN"; echo "KEY=\"$KEY\""
  for l in "$@"; do echo "$l"; done; } > "$conf"
"$v" "$node" dcl 'create/directory [.root.bin]' 'create/directory [.root.com]' 'create/directory [.root.conf]' \
    'create/directory [.root.data]' 'create/directory [.root.log]' \
    'set file/protection=(s:rwed,o:rwed,g,w) [.root]conf.dir,data.dir' >/dev/null
"$v" "$node" put "$root/vms/beszel\$run.com" "$root/vms/beszel\$startup.com" -- root/com >/dev/null 2>&1 || true
"$v" "$node" put "$conf" -- root/conf >/dev/null 2>&1 || true
"$v" "$node" dcl "copy/nolog [.build]beszel_agent.exe,beszel_launch.exe [.root.bin]" \
    "rename/nolog [.root.conf]$(basename "$conf") [.root.conf]agent.conf" \
    "purge/nolog [.root.conf]agent.conf" \
    "set file/protection=(s:rwed,o:rwed,g,w) [.root.conf]agent.conf" \
    "directory/protection/size [.root...]" | grep -vE '^\s*$'
rm -f "$conf"
