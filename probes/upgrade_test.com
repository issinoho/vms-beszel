$! upgrade_test.com - vms2 upgrade test (system-wide: approved by the user).
$!   @upgrade_test <kit-dir>   stop the agent, install the new kit over the old
$!                             one, then run the boot path as a boot would:
$!                             BESZEL$USER deassigned, @SYS$STARTUP:BESZEL$STARTUP
$ set noon
$ say = "write sys$output"
$ define sys$error sys$output
$ x = f$setprv("ALL")
$ kitdir = p1
$ say "UT: === before"
$ product show product BESZEL
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STOP
$ say "UT: === PRODUCT INSTALL (upgrade)"
$ product install BESZEL /producer=ISSINOHO /source='kitdir' /options=noconfirm /log
$ say "UT: install status ", $status
$ product show product BESZEL
$ directory/date/owner/protection SYS$COMMON:[SYS$STARTUP]BESZEL$*.COM
$ say "UT: === as at boot: no BESZEL$USER, no agent"
$ if f$trnlnm("BESZEL$USER", "LNM$SYSTEM_TABLE") .nes. "" then -
     deassign/system/executive_mode BESZEL$USER
$ say "UT: BESZEL$USER = """, f$trnlnm("BESZEL$USER", "LNM$SYSTEM_TABLE"), """"
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STATUS
$ say "UT: === @SYS$STARTUP:BESZEL$STARTUP"
$ @SYS$STARTUP:BESZEL$STARTUP
$ say "UT: startup status ", $status
$ say "UT: BESZEL$USER = """, f$trnlnm("BESZEL$USER", "LNM$SYSTEM_TABLE"), """"
$ wait 0:0:30
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL STATUS
$ type BESZEL$ROOT:[LOG]BESZEL_AGENT.LOG
$ exit
