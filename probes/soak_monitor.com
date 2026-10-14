$! soak_monitor.com - Phase 3 soak test: every 10 minutes for P1 hours, append
$! the agent's resource use to [.ROOT.LOG]SOAK.CSV.  Growth in working set or
$! page-file use means a leak; a new PID means the agent restarted.
$! Writes [.SOAK]SOAK.CSV in the work directory ([.ROOT.LOG] belongs to BESZEL).
$ set noon
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ hours = f$integer(p1)
$ if hours .le. 0 then hours = 25
$ rounds = hours * 6
$ x = f$setprv("WORLD")                ! the agent runs as BESZEL: seeing it needs WORLD
$ if f$search("[]SOAK.DIR") .eqs. "" then create/directory [.soak]
$ csv = "[.soak]soak.csv"
$ if f$search(csv) .eqs. ""
$ then
$   open/write f 'csv'
$   write f "time,pid,cpu_10ms,ws_pagelets,proc_pages,global_pages,pgfl_used_pagelets,virtpeak,bufio,dirio,pagefaults"
$   close f
$ endif
$ n = 0
$loop:
$ pid = ""
$ ctx = ""
$find:
$ p = f$pid(ctx)
$ if p .eqs. "" then goto found
$ if f$getjpi(p,"prcnam") .eqs. "BESZEL_AGENT" then pid = p
$ if pid .eqs. "" then goto find
$ if f$type(ctx) .eqs. "PROCESS_CONTEXT" then tmp = f$context("PROCESS", ctx, "CANCEL")
$found:
$ open/append f 'csv'
$ if pid .eqs. ""
$ then
$   write f f$cvtime(,"COMPARISON"), ",none,,,,,,,,,"
$ else
$   used = f$getjpi(pid,"pgflquota") - f$getjpi(pid,"pagfilcnt")
$   write f f$cvtime(,"COMPARISON"), ",", pid, ",", f$getjpi(pid,"cputim"), ",", -
      f$getjpi(pid,"wssize"), ",", f$getjpi(pid,"ppgcnt"), ",", f$getjpi(pid,"gpgcnt"), ",", -
      used, ",", f$getjpi(pid,"virtpeak"), ",", f$getjpi(pid,"bufio"), ",", -
      f$getjpi(pid,"dirio"), ",", f$getjpi(pid,"pageflts")
$ endif
$ close f
$ n = n + 1
$ if n .ge. rounds then exit
$ wait 0:10:00
$ goto loop
