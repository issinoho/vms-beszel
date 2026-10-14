$! BESZEL$STARTUP.COM - start, stop or check the beszel agent.
$!
$!   @BESZEL$ROOT:[COM]BESZEL$STARTUP [START|STOP|RESTART|STATUS]   (default START)
$!
$! START runs BESZEL$RUN.COM as a detached process named BESZEL_AGENT, with
$! its log in BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG (five versions kept).  If the
$! logical name BESZEL$USER names an account, the process runs as that user
$! (which needs the privileges of a system startup); otherwise as the caller.
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
$ if f$trnlnm("BESZEL$ROOT") .eqs. "" then -
     define/process/nolog/translation=concealed BESZEL$ROOT 'physroot'.]
$ stopfile = "BESZEL$ROOT:[DATA]STOP."
$ name = "BESZEL_AGENT"
$ gosub find_pid
$ if action .eqs. "STATUS" then goto status
$ if action .eqs. "STOP" then goto stop
$ if action .eqs. "RESTART" then goto restart
$ if action .eqs. "START" then goto start
$ say "BESZEL$STARTUP: unknown action ''action' (START, STOP, RESTART or STATUS)"
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
$   say "BESZEL$STARTUP: BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE is missing"
$   exit 44
$ endif
$ if f$search("BESZEL$ROOT:[CONF]AGENT.CONF") .eqs. ""
$ then
$   say "BESZEL$STARTUP: there is no BESZEL$ROOT:[CONF]AGENT.CONF; copy AGENT.CONF_TEMPLATE and fill it in"
$   exit 44
$ endif
$ if f$search(stopfile) .nes. "" then delete/nolog 'stopfile';*
$ if f$search("BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG") .nes. "" then -
     purge/nolog/keep=5 BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG
$ uic = ""
$ user = f$trnlnm("BESZEL$USER")
$ if user .nes. "" then -
     uic = "/uic=" + f$fao("!%U", f$identifier(f$edit(user,"upcase,trim"),"name_to_number"))
$ run/detached sys$system:loginout.exe 'uic' /authorize/process_name='name' -
      /input='physroot'.COM]BESZEL$RUN.COM -
      /output='physroot'.LOG]BESZEL_AGENT.LOG -
      /error='physroot'.LOG]BESZEL_AGENT.LOG
$ if .not. $status then exit $status
$ wait 0:0:2
$ gosub find_pid
$ if pid .eqs. ""
$ then
$   say "BESZEL$STARTUP: the agent did not start; see BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG"
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
$! sets pid to the agent's process (visible to us), or ""
$find_pid:
$ pid = ""
$ ctx = ""
$fp_loop:
$ found = f$pid(ctx)
$ if found .eqs. "" then return
$ if f$getjpi(found,"prcnam") .nes. name then goto fp_loop
$ pid = found
$ if f$type(ctx) .eqs. "PROCESS_CONTEXT" then tmp = f$context("PROCESS", ctx, "CANCEL")
$ return
