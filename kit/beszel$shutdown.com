$! BESZEL$SHUTDOWN.COM - stop the beszel agent at system shutdown
$!
$! Usage (as SYSTEM, from SYS$MANAGER:SYSHUTDWN.COM):
$!   @SYS$STARTUP:BESZEL$SHUTDOWN
$!
$! The agent says goodbye to the hub, which then marks the system down at once
$! instead of after its timeout.
$ set noon
$ if f$trnlnm("BESZEL$ROOT", "LNM$SYSTEM_TABLE") .eqs. "" then exit 1
$ if f$search("BESZEL$ROOT:[COM]BESZEL$CONTROL.COM") .eqs. "" then exit 1
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STOP
$ exit 1
