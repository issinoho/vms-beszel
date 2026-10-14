#!/usr/bin/env bash
# conformance.sh - check our encodings with upstream's own Go structs (D6).
#
# Copies the pinned tree to out/conformance (never touching cache/), adds
# tests/conformance/conformance_test.go as internal/vmsconformance, writes the
# fixtures with build/gen_fixtures and runs `go test` with the Go toolchain in
# cache/toolchain.  Module downloads go to cache/gopath.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/upstream.conf"
src=$root/cache/$UPSTREAM_NAME-$UPSTREAM_VERSION
work=$root/out/conformance
go=$root/cache/toolchain/go/bin/go
[ -d "$src" ] || "$root/tools/fetch.sh"
[ -x "$go" ] || { echo "conformance.sh: no Go toolchain in cache/toolchain (DECISIONS.md D10)" >&2; exit 1; }
rm -rf "$work"
mkdir -p "$work/fixtures"
rsync -a --exclude .git --exclude internal/site "$src/" "$work/beszel/"
mkdir -p "$work/beszel/internal/vmsconformance"
cp "$root/tests/conformance/conformance_test.go" "$work/beszel/internal/vmsconformance/"
"$root/build/gen_fixtures" "$work/fixtures"
cd "$work/beszel"
export GOPATH=$root/cache/gopath GOMODCACHE=$root/cache/gopath/mod GOTOOLCHAIN=local \
       GOFLAGS=-mod=mod BZ_FIXTURES=$work/fixtures CGO_ENABLED=0
"$go" test -count=1 -v ./internal/vmsconformance/
