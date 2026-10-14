#!/usr/bin/env bash
# push.sh - copy the sources and build procedure to a node's work directory.
#   push.sh <node>
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
v=$root/tools/vms.sh
n=${1:?node}
"$v" "$n" put "$root/build_vms.com"
for d in src/core src/plat src/plat/stub src/main tests/unit; do
    "$v" "$n" putdir "$root/$d" "$d" 2>&1 | grep -v 'Couldn.t \(create\|stat\)\|is not a regular file' || true
done
