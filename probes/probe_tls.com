$! probe_tls.com - build and run PROBE_TLS without privileges.
$! P1 hub host, P2 hub port, P3 TLS test host (optional)
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ set default [.probes]
$ define/process/nolog openssl ssl3$include:
$ cc/prefix=all probe_tls
$ if .not. $status then exit
$ open/write o ssl.opt
$ write o "SYS$LIBRARY:SSL3$LIBSSL_SHR32/SHARE"
$ write o "SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32/SHARE"
$ close o
$ link probe_tls,ssl.opt/options
$ if .not. $status then exit
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ write sys$output "#### UNPRIVILEGED: ", f$getjpi("","CURPRIV")
$ pt := $sys$disk:[]probe_tls
$ pt 'p1' 'p2' 'p3'
$ exit
