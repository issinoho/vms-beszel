$! BESZEL$RUN.COM - the beszel agent's detached process (run by BESZEL$STARTUP.COM).
$!
$! Runs BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE and starts it again if it exits,
$! waiting 15 s, then twice as long after each quick failure (up to 5 minutes);
$! a run of 10 minutes or more resets the wait.  It stops for good when
$! BESZEL$ROOT:[DATA]STOP. exists (BESZEL$STARTUP STOP creates it).
$!
$! BESZEL$ROOT is taken from this procedure's own location when it isn't
$! defined, so a tree anywhere works without a system logical name.
$ set noon
$ say = "write sys$output"
$ set process/privileges=(noall,netmbx,tmpmbx)  ! all the agent ever needs (D8)
$! BESZEL$ROOT from the system table only (not job or group tables: review
$! L3), else from this procedure's location; pinned in the process table
$ root = f$trnlnm("BESZEL$ROOT", "LNM$SYSTEM_TABLE", , "EXECUTIVE")
$ if root .eqs. ""
$ then
$   proc = f$environment("procedure")
$   dev = f$parse(proc,,,"device","no_conceal")
$   dir = f$parse(proc,,,"directory","no_conceal") - "][" - "><"
$   root = dev + (dir - ".COM]") + ".]"
$ endif
$ define/process/nolog/translation=concealed BESZEL$ROOT 'root'
$ say "BESZEL$RUN starting at ", f$time(), " as ", f$getjpi("","username"), -
      " with ", f$getjpi("","curpriv")
$ agent := $BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE
$ delay = 15
$loop:
$ if f$search("BESZEL$ROOT:[DATA]STOP.") .nes. "" then goto done
$ started = f$time()
$ say "--- agent starting at ", f$time()
$ agent
$ status = $status
$ say "--- agent exited at ", f$time(), " status ", status
$ if f$search("BESZEL$ROOT:[DATA]STOP.") .nes. "" then goto done
$ if status .eq. 44                    ! SS$_ABORT: the agent refused this account (D12)
$ then
$   say "--- the agent refuses to run as this account (see above): not restarting"
$   goto done
$ endif
$! a long run resets the wait; quick failures back off
$ if f$cvtime("''started'+0-00:10:00.00","COMPARISON") .les. f$cvtime("","COMPARISON") then delay = 15
$ say "--- restarting in ", delay, " s"
$ mins = delay / 60
$ secs = delay - mins * 60
$ wait 0:'mins':'secs'
$ delay = delay * 2
$ if delay .gt. 300 then delay = 300
$ goto loop
$done:
$ say "BESZEL$RUN stopped at ", f$time()
$ exit
