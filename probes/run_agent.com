$! run_agent.com - run BESZEL_AGENT.EXE for P1 seconds with [.RUN]AGENT.CONF,
$! unprivileged, as a subprocess; then stop it (our own process, by name).
$! The output goes to [.RUN]AGENT.LOG.
$ set noon
$ define sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ if f$search("[.RUN]DATA.DIR") .eqs. "" then create/directory [.run.data]
$ secs = f$integer(p1)
$ if secs .le. 0 then secs = 120
$ name = "BZAGENT_" + f$extract(4, 4, f$getjpi("","PID"))
$ conf = f$parse("[.run]agent.conf", here)
$ image = f$parse("[.build]beszel_agent.exe", here)
$ saved = f$setprv("NOALL,NETMBX,TMPMBX")
$ spawn/nowait/process='name'/output=[.run]agent.log mcr 'image' 'conf'
$ saved2 = f$setprv("ALL")
$ mins = secs / 60
$ rest = secs - mins * 60
$ wait 0:'mins':'rest'
$ stop 'name'
$ wait 0:0:2
$ type [.run]agent.log
$ exit
