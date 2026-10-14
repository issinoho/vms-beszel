$! probe_c99.com - compile and run PROBE_C99 with the agent's qualifiers.
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ set default [.probes]
$ cc/standard=c99/pointer_size=32/float=ieee_float/ieee_mode=denorm_results probe_c99
$ if .not. $status then exit
$ link probe_c99
$ run probe_c99
$ exit
