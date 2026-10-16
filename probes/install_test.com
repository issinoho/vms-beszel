$! install_test.com - Phase 4 install test (system-wide: approved by the user).
$!   @install_test INSTALL <kit-dir>   stop the test-tree agent, install, configure,
$!                                     carry over AGENT.CONF and the fingerprint, start
$!   @install_test REMOVE  <kit-dir>   remove, check what is kept, reinstall, start
$ set noon
$ say = "write sys$output"
$ define sys$error sys$output
$ x = f$setprv("ALL")
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ kitdir = p2
$ if f$edit(p1,"UPCASE") .eqs. "REMOVE" then goto remove
$!
$ say "IT: === stop the test-tree agent"
$ ! the test tree has the procedure under its old name or its new one
$ if f$search("[.ROOT.COM]BESZEL$CONTROL.COM") .nes. "" then @[.root.com]beszel$control stop beszel
$ if f$search("[.ROOT.COM]BESZEL$STARTUP.COM") .nes. "" then @[.root.com]beszel$startup stop beszel
$ ! the old procedure leaves a process BESZEL$ROOT, which would hide the system one (NOTES.md 44)
$ if f$trnlnm("BESZEL$ROOT", "LNM$PROCESS_TABLE") .nes. "" then deassign/process BESZEL$ROOT
$ say "IT: === PRODUCT INSTALL"
$ product install BESZEL /producer=ISSINOHO /source='kitdir' /options=noconfirm /log
$ say "IT: install status ", $status
$ say "IT: BESZEL$ROOT = ", f$trnlnm("BESZEL$ROOT","LNM$SYSTEM_TABLE")
$ say "IT: === mark the test account as the procedure's own"
$ define/user sysuaf sys$system:sysuaf
$ mcr authorize modify BESZEL /owner="beszel agent"
$ say "IT: === BESZEL$CONFIGURE"
$ @BESZEL$ROOT:[COM]BESZEL$CONFIGURE BESZEL "" YES
$ say "IT: configure status ", $status
$ say "IT: === carry over AGENT.CONF and the fingerprint"
$ copy/nolog [.root.conf]agent.conf;0 BESZEL$ROOT:[CONF]AGENT.CONF
$ purge/nolog BESZEL$ROOT:[CONF]AGENT.CONF
$ copy/nolog [.root.data]fingerprint.;0 BESZEL$ROOT:[DATA]FINGERPRINT.
$ set file/owner=BESZEL/protection=(s:rwed,o:rwed,g,w) BESZEL$ROOT:[CONF]AGENT.CONF;*,BESZEL$ROOT:[DATA]FINGERPRINT.;*
$ directory/owner/protection BESZEL$ROOT:[000000...]
$ type SYS$MANAGER:BESZEL$CONFIG.COM
$ say "IT: BESZEL$USER = ", f$trnlnm("BESZEL$USER","LNM$SYSTEM_TABLE")
$ say "IT: === START"
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL START
$ wait 0:0:30
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STATUS
$ type BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG
$ exit
$!
$remove:
$ root = f$trnlnm("BESZEL$ROOT","LNM$SYSTEM_TABLE")
$ say "IT: === PRODUCT REMOVE (root ", root, ")"
$ product remove BESZEL /options=noconfirm /log
$ say "IT: remove status ", $status
$ say "IT: BESZEL$ROOT after remove = """, f$trnlnm("BESZEL$ROOT","LNM$SYSTEM_TABLE"), """, BESZEL$USER = """, -
      f$trnlnm("BESZEL$USER","LNM$SYSTEM_TABLE"), """"
$ say "IT: agent processes after remove (none expected):"
$ pipe show system | search sys$pipe BESZEL_AGENT
$ dev = f$element(0, "[", root)
$ dir = "[" + (f$element(1, "[", root) - ".]") + "...]"
$ say "IT: what is left in ", dev, dir
$ directory/owner/protection 'dev''dir'
$ say "IT: === reinstall"
$ product install BESZEL /producer=ISSINOHO /source='kitdir' /options=noconfirm /log
$ @BESZEL$ROOT:[COM]BESZEL$CONFIGURE BESZEL "" YES
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL START
$ wait 0:0:30
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STATUS
$ type BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG
$ exit
