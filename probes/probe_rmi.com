$! probe_rmi.com - build and run PROBE_RMI without privileges.
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ set default [.probes]
$ cc probe_rmi
$ if .not. $status then exit
$ link probe_rmi
$ if .not. $status then exit
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ write sys$output "#### UNPRIVILEGED: ", f$getjpi("","CURPRIV")
$ run probe_rmi

$! (second pass, appended for the XFC question)
$ saved2 = f$setprv("ALL")
$ write sys$output "#### PRIVILEGED"
$ run probe_rmi
$ exit
