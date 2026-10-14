$! probe_xfc.com - build and run PROBE_XFC unprivileged, then show the cache.
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ set default [.probes]
$ cc probe_xfc
$ link probe_xfc
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ run probe_xfc
$ saved2 = f$setprv("ALL")
$ write sys$output "#### PRIVILEGED"
$ run probe_xfc
$ pipe show memory/cache | search sys$pipe "Allocated","In use"
$ exit
