#!/usr/bin/env bash
# fetch.sh - clone the pinned upstream beszel tree into cache/beszel-<ver>.
# Refuses a tag that does not point at UPSTREAM_COMMIT.  Never edit the result.
#
#   fetch.sh          the source tree (the specification)
#   fetch.sh --ref    also upstream's linux/amd64 release binaries (hub and agent) into
#                     cache/ref/, checked against the release's checksums file (D10)
#   fetch.sh --ca     the kit's CA bundle into cache/cacert.pem, checked against
#                     CA_BUNDLE_SHA256
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=/dev/null
. "$root/upstream.conf"
dest=$root/cache/$UPSTREAM_NAME-$UPSTREAM_VERSION
if [ "${1:-}" = --ca ]; then
    out=$root/cache/cacert.pem
    if [ -f "$out" ] && echo "$CA_BUNDLE_SHA256  $out" | sha256sum -c --status; then
        echo "$out (already there)"; exit 0
    fi
    mkdir -p "$root/cache"
    curl -fsSL -o "$out.tmp" "$CA_BUNDLE_URL"
    echo "$CA_BUNDLE_SHA256  $out.tmp" | sha256sum -c --status ||
        { rm -f "$out.tmp"; echo "fetch.sh: $CA_BUNDLE_URL doesn't match CA_BUNDLE_SHA256" >&2; exit 1; }
    mv "$out.tmp" "$out"
    echo "$out ($CA_BUNDLE_URL)"
    exit 0
fi
if [ "${1:-}" = --ref ]; then
    ref=$root/cache/ref
    mkdir -p "$ref"
    sums=${UPSTREAM_NAME}_${UPSTREAM_VERSION}_checksums.txt
    ( cd "$ref"
      gh release download "$UPSTREAM_TAG" -R henrygd/beszel --clobber \
          -p "$sums" -p beszel_linux_amd64.tar.gz -p beszel-agent_linux_amd64.tar.gz
      grep -E ' (beszel|beszel-agent)_linux_amd64\.tar\.gz$' "$sums" | sha256sum -c
      for f in beszel_linux_amd64 beszel-agent_linux_amd64; do
          mkdir -p "$f" && tar xzf "$f.tar.gz" -C "$f"
      done )
    echo "$ref (release binaries $UPSTREAM_TAG)"
fi
if [ -d "$dest/.git" ]; then
    have=$(git -C "$dest" rev-parse HEAD)
    [ "$have" = "$UPSTREAM_COMMIT" ] || { echo "fetch.sh: $dest is at $have, not $UPSTREAM_COMMIT" >&2; exit 1; }
    echo "$dest (already at $UPSTREAM_TAG)"; exit 0
fi
mkdir -p "$root/cache"
tmp=$(mktemp -d "$root/cache/.fetch.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 --branch "$UPSTREAM_TAG" "$UPSTREAM_REPO" "$tmp/src"
have=$(git -C "$tmp/src" rev-parse HEAD)
if [ "$have" != "$UPSTREAM_COMMIT" ]; then
    echo "fetch.sh: tag $UPSTREAM_TAG is $have, upstream.conf pins $UPSTREAM_COMMIT" >&2
    exit 1
fi
mv "$tmp/src" "$dest"
echo "$dest ($UPSTREAM_TAG, $have)"
