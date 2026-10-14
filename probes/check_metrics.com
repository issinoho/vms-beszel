$! check_metrics.com - Phase 2 cross-check: the agent's view against the
$! system's own, over the same 10 s.  P1: extra disks for --sample.
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ image = f$parse("[.build]beszel_agent.exe", here)
$ bz := $'image'
$ mon = "BZMON_" + f$extract(4, 4, f$getjpi("","PID"))
$ spawn/nowait/process='mon' monitor modes/interval=10/ending="+0-00:00:11"/nodisplay/summary=[]modes.sum
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ write sys$output "#### AGENT (", f$getjpi("","CURPRIV"), ")"
$ bz --sample 10 'p1' 'p2' 'p3'
$ saved2 = f$setprv("ALL")
$! MONITOR ends 11 s after it started; the agent took 10 s of that
$ wait 0:0:12
$ write sys$output "#### MONITOR MODES (same 10 s)"
$ type []modes.sum
$ delete []modes.sum;*
$ write sys$output "#### SYSTEM"
$ show memory/physical
$ show memory/files
$ show device d/mounted
$ pipe show system/noprocess
$ pipe mcr lancp show configuration
$ exit
