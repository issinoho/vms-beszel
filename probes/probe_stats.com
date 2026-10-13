$! probe_stats.com - build and run PROBE_STATS twice (no privileges, then
$! authorized privileges), with the system's own views for cross-checking.
$ set noon
$ define/user sys$error sys$output
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ set default [.probes]
$ cc/warnings=disable=(QUESTCOMPARE) probe_stats+sys$library:sys$lib_c.tlb/library
$ if .not. $status then exit
$ link probe_stats
$ if .not. $status then exit
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ write sys$output "#### UNPRIVILEGED: ", f$getjpi("","CURPRIV")
$ run probe_stats
$ saved2 = f$setprv(saved)
$ saved3 = f$setprv("ALL")
$ write sys$output "#### PRIVILEGED: ", f$getjpi("","CURPRIV")
$ ps := $sys$disk:[]probe_stats
$ ps 5
$ write sys$output "#### SYSTEM VIEWS"
$ show memory/physical
$ show memory/files
$ show memory/cache
$ show device d/mounted
$ mcr lancp show configuration
$ write sys$output "#### TCPIP"
$ tcpip show interface
$ write sys$output "#### MONITOR MODES 10s"
$ monitor modes/interval=5/ending="+0-00:00:11"/nodisplay/summary=modes.sum
$ type modes.sum
$ delete modes.sum;*
$ exit
