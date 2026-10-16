$! BESZEL$CONTROL.COM - start, stop or check the beszel agent.
$!
$!   @BESZEL$ROOT:[COM]BESZEL$CONTROL [START|STOP|RESTART|STATUS] [account]   (default START)
$!
$! START runs BESZEL$RUN.COM as a detached process named BESZEL_AGENT, with
$! its log in BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG (five versions kept).  If the
$! account is given (P2), or else the system logical name BESZEL$USER names one,
$! the process runs as that user (which needs the privileges of a system
$! startup); otherwise as the caller.  It refuses an unknown account and any system-group UIC (D12).
$! BESZEL$ROOT and BESZEL$USER are read from the system table only, never from
$! job or group tables other users could write (security review L3).
$! STOP asks the agent to stop (the file BESZEL$ROOT:[DATA]STOP.), waits up to
$! 20 s for it to close its connection, and only then stops the process.
$ set noon
$ say = "write sys$output"
$ action = f$edit(p1, "upcase,trim")
$ if action .eqs. "" then action = "START"
$ proc = f$environment("procedure")
$ dev = f$parse(proc,,,"device","no_conceal")
$ dir = f$parse(proc,,,"directory","no_conceal") - "][" - "><"
$ physroot = dev + (dir - ".COM]")     ! "DEV:[A.B" - add ".X]" for a subdirectory
$ sysroot = f$trnlnm("BESZEL$ROOT", "LNM$SYSTEM_TABLE", , "EXECUTIVE")
$ if sysroot .eqs. "" then sysroot = physroot + ".]"
$ define/process/nolog/translation=concealed BESZEL$ROOT 'sysroot'
$ user = f$edit(p2, "upcase,trim")
$ if user .eqs. "" then user = f$edit(f$trnlnm("BESZEL$USER", "LNM$SYSTEM_TABLE"), "upcase,trim")
$ if user .eqs. "" then user = f$edit(f$getjpi("", "username"), "upcase,trim")
$ stopfile = "BESZEL$ROOT:[DATA]STOP."
$ name = "BESZEL_AGENT"
$ gosub find_pid
$ if action .eqs. "STATUS" then goto status
$ if action .eqs. "STOP" then goto stop
$ if action .eqs. "RESTART" then goto restart
$ if action .eqs. "START" then goto start
$ say "BESZEL$CONTROL: unknown action ''action' (START, STOP, RESTART or STATUS)"
$ exit 44
$!
$start:
$ if pid .nes. ""
$ then
$   say "BESZEL_AGENT is already running (PID ''pid')"
$   exit 1
$ endif
$ if f$search("BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE") .eqs. ""
$ then
$   say "BESZEL$CONTROL: BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE is missing"
$   exit 44
$ endif
$ if f$search("BESZEL$ROOT:[CONF]AGENT.CONF") .eqs. ""
$ then
$   say "BESZEL$CONTROL: there is no BESZEL$ROOT:[CONF]AGENT.CONF; copy AGENT.CONF_TEMPLATE and fill it in"
$   exit 44
$ endif
$! D12: never a system account (the agent checks its privileges itself)
$ uicnum = f$identifier(user, "name_to_number")
$ if uicnum .eq. 0
$ then
$   say "BESZEL$CONTROL: ''user' is not an account"
$   exit 44
$ endif
$ if uicnum / 65536 .le. f$getsyi("MAXSYSGROUP")
$ then
$   say "BESZEL$CONTROL: refusing to run the agent as ''user', which has a system UIC; define"
$   say "  the system logical name BESZEL$USER as an account of its own (see the README)"
$   exit 44
$ endif
$ if f$search(stopfile) .nes. "" then delete/nolog 'stopfile';*
$ if f$search("BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG") .nes. "" then -
     purge/nolog/keep=5 BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG
$ if user .eqs. f$edit(f$getjpi("", "username"), "upcase,trim")
$ then
$   run/detached sys$system:loginout.exe /authorize/process_name='name' -
        /input='physroot'.COM]BESZEL$RUN.COM -
        /output='physroot'.LOG]BESZEL_AGENT.LOG -
        /error='physroot'.LOG]BESZEL_AGENT.LOG
$ else
$!  As another account: RUN/DETACHED/UIC would keep our username and our
$!  authorized privileges (NOTES.md 36), so the launcher takes on the
$!  account's persona and creates the process under it (D14).
$   launch := $'physroot'.BIN]BESZEL_LAUNCH.EXE
$   launch 'user' 'physroot'.COM]BESZEL$RUN.COM 'physroot'.LOG]BESZEL_AGENT.LOG 'name'
$ endif
$ if .not. $status then exit $status
$ wait 0:0:2
$ gosub find_pid
$ if pid .eqs. ""
$ then
$   say "BESZEL$CONTROL: the agent did not start; see BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG"
$   exit 44
$ endif
$ say "BESZEL_AGENT started (PID ''pid')"
$ exit 1
$!
$stop:
$ if pid .eqs. ""
$ then
$   say "BESZEL_AGENT is not running"
$   if action .eqs. "RESTART" then return
$   exit 1
$ endif
$ open/write f 'stopfile'
$ close f
$ n = 0
$stop_wait:
$ wait 0:0:1
$ gosub find_pid
$ if pid .eqs. "" then goto stopped
$ n = n + 1
$ if n .lt. 20 then goto stop_wait
$ say "BESZEL_AGENT did not stop in 20 s: stopping process ''pid'"
$ stop/id='pid'
$stopped:
$ say "BESZEL_AGENT stopped"
$ if action .eqs. "RESTART" then return
$ exit 1
$!
$restart:
$ if pid .nes. "" then gosub stop
$ goto start
$!
$status:
$ if pid .eqs. ""
$ then
$   say "BESZEL_AGENT is not running"
$ else
$   say "BESZEL_AGENT is running: PID ", pid, ", user ", f$edit(f$getjpi(pid,"username"),"trim"), -
        ", CPU ", f$getjpi(pid,"cputim")/100, " s, working set ", f$getjpi(pid,"wssize"), " pagelets"
$ endif
$ exit 1
$!
$! sets pid to the agent's process, or "": the name AND the account must
$! match, so another user's BESZEL_AGENT can't be mistaken for it (review L4)
$find_pid:
$ pid = ""
$ ctx = ""
$fp_loop:
$ found = f$pid(ctx)
$ if found .eqs. "" then return
$ if f$getjpi(found,"prcnam") .nes. name then goto fp_loop
$ if f$edit(f$getjpi(found,"username"),"upcase,trim") .nes. user then goto fp_loop
$ pid = found
$ if f$type(ctx) .eqs. "PROCESS_CONTEXT" then tmp = f$context("PROCESS", ctx, "CANCEL")
$ return
