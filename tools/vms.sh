#!/usr/bin/env bash
# vms.sh - run things on the VMS build nodes listed in tools/nodes.conf.
#
#   vms.sh <node> put    <local-file>... [-- <subdir>]  upload files (to workdir or workdir/subdir)
#   vms.sh <node> putdir <local-dir> <subdir>           upload every file in a directory (flat)
#   vms.sh <node> putr   <local-dir> <remote-dir>       upload a directory tree (VMS_SFTPDIR: base)
#   vms.sh <node> get    <remote-relpath> <local-file>  download a file relative to the workdir
#   vms.sh <node> dcl    '<dcl command>'                run one DCL command, print its output
#   vms.sh <node> run    <local.com> [params...]        upload + run a procedure interactively
#   vms.sh <node> batch  <local.com> [params...]        upload + SUBMIT, wait for the log, print it
#                                                       ($VMS_SUBMIT_QUALS adds qualifiers)
#
# Output from `ssh host <cmd>` is unreliable on these nodes, so everything runs
# with /OUTPUT (or a batch log) into the workdir and the log is fetched by sftp.
# dcl and run exit 1 when the procedure's log never shows its end line.
#
# One copy of this file is shared, unchanged, by every repository of the vms-*
# family: change it in all of them, never in one.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# A repository that has hostenv.sh (bash version, host tool checks) uses it.
[ -f "$here/hostenv.sh" ] && . "$here/hostenv.sh"
conf=$here/nodes.conf
key=${VMS_SSH_KEY:-$HOME/.ssh/vms_ed25519}

usage() { awk 'NR > 1 && /^#/ { print; next } NR > 1 { exit }' "$0" >&2; exit 2; }
[ $# -ge 2 ] || usage
node=$1 op=$2; shift 2

# Columns past the 7th are a repository's own (vms-mariadb's PCRE2 tree):
# the trailing _ takes them, so they never run into SFTPDIR.
read -r _ ARCH HOST PORT USER WORKDIR SFTPDIR _ < <(awk -v n="$node" '$1==n' "$conf") || true
[ -n "${HOST:-}" ] || { echo "vms.sh: unknown node '$node' (see $conf)" >&2; exit 2; }

# ConnectTimeout 60: x86's sshd starts a job per connection and took over 15 s
# to answer under load (vms-vftp, 2026-10-30).
ssh_opts=(-i "$key" -o BatchMode=yes -o ConnectTimeout="${VMS_CONNECT_TIMEOUT:-60}"
          -o ServerAliveInterval=30)

# A failure is reported, never silent: under set -e and pipefail a bare call
# used to end vms.sh with status 1 and no message (vms-mariadb, 2026-10-13).
# Callers that probe for a file and expect failure send stderr to /dev/null.
sftp_batch() {  # sftp_batch <commands...>
    local rc=0
    printf '%s\n' "$@" | sftp -P "$PORT" "${ssh_opts[@]}" -b - "$USER@$HOST" 2>&1 >/dev/null |
        { grep -vE '^ *Welcome to|^ *$' >&2 || true; } || rc=$?
    [ "$rc" -eq 0 ] || echo "vms.sh: sftp to $node failed (status $rc): $(printf '%s; ' "$@")" >&2
    return "$rc"
}

# DISK$USER:[USERNAME.PROJECT] + sub/dir -> DISK$USER:[USERNAME.PROJECT.SUB.DIR]
vms_dir() {
    local sub=${1:-}
    if [ -z "$sub" ]; then echo "$WORKDIR"; return; fi
    echo "${WORKDIR%]}.$(echo "$sub" | tr '/' '.' | tr '[:lower:]' '[:upper:]')]"
}

# Run DCL command(s) with output captured to a log, then print the log.
#
# The procedure writes <tag>.STARTED as its first act and <tag>.DONE as its
# last. The host waits for DONE, not for ssh to exit: the node's sshd sometimes
# keeps the session open after the process has finished, and sometimes ends it
# while the process runs on without it (AXPBOX, 2026-11-11; IA64).
#
# When ssh ends with no DONE, only STARTED reliably tells a procedure that never
# began (IA64, 2026-10-16: ssh gone in 2 s, only the .com there), which is safe
# to start again, from one still running, which is not. The log is no test: a
# running procedure holds it open, sftp cannot read it and has been seen not to
# list it (vms-mariadb, 2026-10-13), and treating a busy procedure as unstarted
# ran one twice (Alpha, 2026-11-09: two .DONE versions for one tag).
#
# Nothing is deleted on the node until the log is complete: DCL reopens the
# wrapper when a nested procedure returns, and with the file gone it stops with
# %RMS-E-FNF and %RMS-F-ISI (vms-mariadb probes/ssh_drop, 2026-10-13).
dcl_logged() {
    local tag=vmsrun_$$_$RANDOM
    local com out mark sshlog
    com=$(mktemp) out=$(mktemp) mark=$(mktemp) sshlog=$(mktemp)
    { echo '$ set noon'
      echo "\$ open/write vmsrun_mark ${WORKDIR}${tag}.STARTED"
      echo '$ close vmsrun_mark'
      echo "\$ set default $WORKDIR"; printf '%s\n' "$@"
      echo '$ write sys$output "VMSRUN-END"'
      echo "\$ open/write vmsrun_mark ${WORKDIR}${tag}.DONE"
      echo '$ close vmsrun_mark'; } > "$com"
    sftp_batch "cd $SFTPDIR" "put $com $tag.com"

    have() { sftp_batch "cd $SFTPDIR" "get $tag.$1 $mark" 2>/dev/null; }
    ssh_said() {
        tr -d '\r' < "$sshlog" | grep -avE '^ *Welcome to|post-quantum|^ *$' | head -"$1" || true
    }
    # ssh itself in the background, so that $pid is ssh's and the kill below ends it.
    # (It was a { ssh; echo; } group: the kill ended the subshell and left ssh running,
    # still connected, holding an SSHD process on the node: 45 orphans by 2026-10-17.)
    # ssh is always ended here, also if vms.sh itself is ended (timeout, Ctrl-C).
    start_ssh() {
        ssh -p "$PORT" "${ssh_opts[@]}" "$USER@$HOST" \
            "@${WORKDIR}${tag}.COM/OUTPUT=${WORKDIR}${tag}.LOG" </dev/null >"$sshlog" 2>&1 &
        pid=$!
    }
    local pid waited=0 limit=${VMS_TIMEOUT:-600} starts=1 state=running ssh_gone=0
    trap 'kill "${pid:-}" 2>/dev/null || true' EXIT
    trap 'kill "${pid:-}" 2>/dev/null; exit 143' TERM INT HUP
    start_ssh
    while :; do
        sleep 2; waited=$((waited + 2))
        if [ "$ssh_gone" = 0 ] && ! kill -0 "$pid" 2>/dev/null; then
            # Give a starting procedure a moment to write STARTED.
            have DONE && { state=done; break; }
            sleep 5; waited=$((waited + 5))
            have DONE && { state=done; break; }
            if ! have STARTED; then
                # Nothing ran, so starting it again cannot run anything twice. Wait
                # first: IA64 sometimes refuses a session just after an sftp one.
                if [ "$starts" -ge 3 ]; then state=notstarted; break; fi
                echo "vms.sh: $tag did not start on $node; ssh said: $(ssh_said 2 | tr '\n' ' ')- starting it again" >&2
                starts=$((starts + 1)); sleep 10; waited=$((waited + 10))
                start_ssh; continue
            fi
            ssh_gone=$waited
            echo "vms.sh: the ssh session to $node ended after ${ssh_gone}s; the procedure" \
                 "still runs there: waiting for it (up to ${limit}s)" >&2
        fi
        # Poll at 2 and 4 s for a short command whose session the node holds open,
        # then every 10 s while ssh is up, every 20 s once it has gone.
        if { [ "$waited" -le 4 ] || [ $((waited % (ssh_gone ? 20 : 10))) -lt 2 ]; } &&
            have DONE; then state=done; break; fi
        [ "$waited" -ge "$limit" ] && { state=timeout; break; }
    done
    kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true

    case $state in
    notstarted)
        echo "vms.sh: no session started on $node after $starts tries; ssh said:" >&2
        ssh_said 5 >&2
        sftp_batch "cd $SFTPDIR" "-rm $tag.com" >/dev/null 2>&1 || true
        rm -f "$com" "$out" "$mark" "$sshlog"
        return 1 ;;
    timeout)
        # It may still be running: leave its files alone (see above).
        echo "vms.sh: timed out after ${limit}s; the procedure may still be running on $node:" >&2
        echo "vms.sh:   its log is ${WORKDIR}${tag}.LOG (readable when it ends)" >&2
        rm -f "$com" "$out" "$mark" "$sshlog"
        return 1 ;;
    esac

    # The log is closed only when the process exits, after the DONE marker:
    # fetch until it holds the end line written just before the marker.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sftp_batch "cd $SFTPDIR" "get $tag.LOG $out" 2>/dev/null &&
            grep -aq '^VMSRUN-END' "$out" && break
        sleep 3
    done
    local rc=0
    if grep -aq '^VMSRUN-END' "$out"; then
        sftp_batch "cd $SFTPDIR" "-rm $tag.com" "-rm $tag.LOG" "-rm $tag.STARTED" "-rm $tag.DONE" || true
    else
        echo "vms.sh: could not read the log ${WORKDIR}${tag}.LOG (left on the node)" >&2; rc=1
    fi
    tr -d '\r' < "$out" | grep -av '^VMSRUN-END' || true
    rm -f "$com" "$out" "$mark" "$sshlog"
    return $rc
}

case $op in
put)
    files=() sub=
    while [ $# -gt 0 ]; do
        if [ "$1" = -- ]; then sub=$2; break; fi
        files+=("$1"); shift
    done
    cmds=("cd $SFTPDIR")
    [ -n "$sub" ] && cmds+=("-mkdir $sub" "cd $sub")
    for f in "${files[@]}"; do cmds+=("put $f"); done
    sftp_batch "${cmds[@]}"
    ;;
putdir)
    [ $# -eq 2 ] || usage
    sftp_batch "cd $SFTPDIR" "-mkdir $2" "cd $2" "lcd $1" "mput *"
    ;;
putr)
    # a directory tree, e.g. phpBB for vms-lighttpd's Phase 4 board; VMS_SFTPDIR
    # names another base directory (another repository's nodes.conf column 7)
    [ $# -eq 2 ] || usage
    sftp_batch "cd ${VMS_SFTPDIR:-$SFTPDIR}" "put -r $1 $2"
    ;;
get)
    [ $# -eq 2 ] || usage
    sftp_batch "cd $SFTPDIR" "get $1 $2"
    ;;
dcl)
    [ $# -ge 1 ] || usage
    cmds=()
    for c in "$@"; do cmds+=("\$ $c"); done
    dcl_logged "${cmds[@]}"
    ;;
run)
    [ $# -ge 1 ] || usage
    com=$1; shift
    sftp_batch "cd $SFTPDIR" "put $com"
    params=""
    for p in "$@"; do params+=" \"$p\""; done
    dcl_logged "\$ @${WORKDIR}$(basename "$com")$params"
    ;;
batch)
    [ $# -ge 1 ] || usage
    com=$1; shift
    name=$(basename "$com" .com)
    log=${name}_batch.log
    sftp_batch "cd $SFTPDIR" "put $com" "-rm $log"
    plist=""
    for p in "$@"; do plist+="${plist:+,}\"$p\""; done
    # VMS_SUBMIT_QUALS: more SUBMIT qualifiers, e.g. /CPUTIME=00:30:00 to stop a runaway.
    dcl_logged "\$ submit/noprint${VMS_SUBMIT_QUALS:-}/log_file=${WORKDIR}${log}${plist:+/parameters=($plist)} ${WORKDIR}$(basename "$com")" >&2
    # The log is held open by the batch job until it ends; sftp cannot read it until then.
    out=$(mktemp)
    for _ in $(seq 1 "${VMS_BATCH_POLLS:-360}"); do
        if sftp_batch "cd $SFTPDIR" "get $log $out" 2>/dev/null && [ -s "$out" ]; then
            tr -d '\r' < "$out"; rm -f "$out"; exit 0
        fi
        sleep "${VMS_BATCH_POLL_SECS:-20}"
    done
    echo "vms.sh: timed out waiting for batch log $log" >&2; rm -f "$out"; exit 1
    ;;
*) usage ;;
esac
