#!/usr/bin/env bash
# kit.sh <node> - build the agent on <node>, run its native tests, make the PCSI
# kit there and fetch it to out/kits/, with SHA256SUMS.
#
# The versions come from src/core/bz_version.h alone; MAKE_KIT.COM refuses to
# package an image that reports a different one.
set -euo pipefail

top=$(cd "$(dirname "$0")/.." && pwd)
node=${1:?usage: kit.sh <node>}
: "${KIT_PRODUCER:=ISSINOHO}"
KIT_PRODUCT=BESZEL
version=$(sed -n 's/^#define BZ_PORT_VERSION *"\(.*\)"/\1/p' "$top/src/core/bz_version.h")
pcsiversion=$(sed -n 's/^#define BZ_PCSI_VERSION *"\(.*\)"/\1/p' "$top/src/core/bz_version.h")
[ -n "$version" ] && [ -n "$pcsiversion" ] || { echo "kit: no version in bz_version.h" >&2; exit 1; }
read -r _ ARCH _ _ _ WORKDIR _ < <(awk -v n="$node" '$1==n' "$top/tools/nodes.conf")
case ${ARCH^^} in X86*) base=X86VMS; archname=x86-64 ;; ALPHA*|AXP*) base=AXPVMS; archname=Alpha ;;
    IA64*|I64*) base=I64VMS; archname=Integrity ;; *) echo "kit: unknown architecture '$ARCH' in tools/nodes.conf" >&2; exit 1 ;; esac
# One kit at a time: every build shares out/kitstage.
mkdir -p "$top/out"; exec 9> "$top/out/.kit.lock"
flock -n 9 || { echo "kit: another kit.sh is running (out/kitstage is shared)" >&2; exit 1; }
"$top/tools/fetch.sh" --ca > /dev/null

# Generated kit inputs: never committed (they carry the version), uploaded to [.KIT].
stage=$top/out/kitstage
rm -rf "$stage"; mkdir -p "$stage" "$top/out/kits"
subst() { sed -e "s/@PRODUCER@/$KIT_PRODUCER/g" -e "s/@BASE@/$base/g" \
              -e "s/@PCSIVERSION@/$pcsiversion/g" -e "s/@VERSION@/$version/g" -e "s/@ARCH@/$archname/g"; }
subst < "$top/kit/beszel.pcsi\$desc_template" > "$stage/PRODUCT-$base.PCSI\$DESC"
subst < "$top/kit/beszel.pcsi\$text_template" > "$stage/PRODUCT-$base.PCSI\$TEXT"
subst < "$top/kit/readme.vms" > "$stage/README.VMS"
cp "$top/cache/cacert.pem" "$stage/CACERT.PEM"
printf 'KIT_PRODUCER=%s\nKIT_PRODUCT=%s\nPCSI_VERSION=%s\nKIT_VERSION=%s\n' \
    "$KIT_PRODUCER" "$KIT_PRODUCT" "$pcsiversion" "$version" > "$stage/KIT.ENV"

echo "kit: beszel agent $version ($KIT_PRODUCER $base $KIT_PRODUCT $pcsiversion) on $node"
"$top/tools/push.sh" "$node" > /dev/null
"$top/tools/vms.sh" "$node" put "$top/LICENSE" > /dev/null 2>&1 || true
"$top/tools/vms.sh" "$node" put "$top/vms/agent.conf_template" "$top/vms/beszel\$control.com" \
    "$top/vms/beszel\$run.com" -- vms > /dev/null 2>&1 || true
"$top/tools/vms.sh" "$node" put "$stage"/* "$top/kit/make_kit.com" "$top/kit/beszel\$configure.com" \
    "$top/kit/beszel\$startup.com" "$top/kit/beszel\$shutdown.com" -- kit > /dev/null 2>&1 || true
# Build, test and package as batch jobs: an interactive ssh session to the Alpha
# can end, or never start the procedure, and lose the output (NOTES.md 50).
batch_com() {  # batch_com <name> <dcl line> - a procedure for vms.sh batch, in out/
    printf '$ set noon\n$ define sys$error sys$output\n$ set default %s\n$ %s\n' \
        "$WORKDIR" "$2" > "$top/out/$1.com"
}
batch_com kit_build '@build_vms TEST'
VMS_BATCH_POLLS=90 "$top/tools/vms.sh" "$node" batch "$top/out/kit_build.com" > "$top/out/kit-build-$node.log" 2>&1
grep -q '0 failed' "$top/out/kit-build-$node.log" && grep -q 'BUILD-OK' "$top/out/kit-build-$node.log" ||
    { tail -20 "$top/out/kit-build-$node.log"; echo "kit: build or native tests failed" >&2; exit 1; }
grep -E 'checks' "$top/out/kit-build-$node.log" || true
batch_com kit_make '@[.kit]make_kit'
VMS_BATCH_POLLS=90 "$top/tools/vms.sh" "$node" batch "$top/out/kit_make.com" 2>&1 | tr -d '\r' | tee "$top/out/kit-$node.log"
kit=$(sed -n 's/^MAKE_KIT: kit .*\]\([^;]*\);.*/\1/p' "$top/out/kit-$node.log" | head -1)
[ -n "$kit" ] || { echo "kit: no kit produced (see out/kit-$node.log)" >&2; exit 1; }
arch=$("$top/tools/vms.sh" "$node" dcl 'write sys$output f$edit(f$getsyi("ARCH_NAME"),"UPCASE")' | tr -d '\r' | grep -E '^[A-Z0-9_]+$' | head -1 || true)
[ -n "$arch" ] || arch=${ARCH^^}   # nodes.conf's arch is the node's ARCH_NAME, upper case
rm -f "$top/out/kits/$kit"     # so a failed fetch can't leave the previous kit in SHA256SUMS
"$top/tools/vms.sh" "$node" get "KIT_$arch/$kit" "$top/out/kits/$kit"
[ -s "$top/out/kits/$kit" ] || { echo "kit: couldn't fetch $kit from $node" >&2; exit 1; }
(cd "$top/out/kits" && sha256sum ISSINOHO-*-BESZEL-*.PCSI > SHA256SUMS && cat SHA256SUMS)
