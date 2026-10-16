$! BESZEL$STARTUP.COM - the beszel agent for OpenVMS at system startup
$!
$! Usage (as SYSTEM):
$!   @SYS$STARTUP:BESZEL$STARTUP          from SYSTARTUP_VMS.COM: define BESZEL$ROOT
$!                                        and BESZEL$USER, and start the agent if
$!                                        SYS$MANAGER:BESZEL$CONFIG.COM says AUTOSTART
$!   @SYS$STARTUP:BESZEL$STARTUP INSTALL  define BESZEL$ROOT only (PCSI postinstall)
$!   @SYS$STARTUP:BESZEL$STARTUP REMOVE   stop the agent, deassign (PCSI remove)
$!
$! The agent runs as its own unprivileged account (DECISIONS.md D12, D14); this
$! procedure needs the privileges of a system startup to start it as that account.
$!
$ set noon
$ op = f$edit(p1, "UPCASE,TRIM")
$ if op .eqs. "REMOVE" then goto remove
$! This procedure sits in <destination>[SYS$STARTUP]; the product is in
$! <destination>[BESZEL].
$ proc = f$environment("PROCEDURE")
$ dev = f$parse(proc,,,"DEVICE","NO_CONCEAL")
$ dir = f$edit(f$parse(proc,,,"DIRECTORY","NO_CONCEAL"), "UPCASE") - "]["
$ root = dir - "SYS$STARTUP]" + "BESZEL.]"
$ if root .eqs. dir + "BESZEL.]"
$ then
$   write sys$error "BESZEL$STARTUP: expected to be in a [SYS$STARTUP] directory, not ''dir'"
$   exit 44
$ endif
$ root = dev + (root - ".000000")
$ define/system/executive_mode/translation_attributes=concealed BESZEL$ROOT 'root'
$ if f$search("BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE") .eqs. ""
$ then
$   write sys$error "BESZEL$STARTUP: no BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE (root ", root, ")"
$   exit 44
$ endif
$ if op .eqs. "INSTALL" then exit 1
$!
$! boot: the site settings name the account, and say whether to start
$ if f$search("SYS$MANAGER:BESZEL$CONFIG.COM") .eqs. "" then exit 1
$ beszel_account = ""
$ beszel_autostart = "NO"
$ @SYS$MANAGER:BESZEL$CONFIG.COM
$ if beszel_account .nes. "" then -
     define/system/executive_mode BESZEL$USER 'f$edit(beszel_account, "UPCASE,TRIM")'
$ if f$edit(beszel_autostart, "UPCASE,TRIM") .nes. "YES" then exit 1
$ @BESZEL$ROOT:[COM]BESZEL$CONTROL START
$ exit 1
$!
$remove:
$ if f$trnlnm("BESZEL$ROOT", "LNM$SYSTEM_TABLE") .nes. ""
$ then
$   if f$search("BESZEL$ROOT:[COM]BESZEL$CONTROL.COM") .nes. "" then -
       @BESZEL$ROOT:[COM]BESZEL$CONTROL STOP
$   deassign/system/executive_mode BESZEL$ROOT
$ endif
$ if f$trnlnm("BESZEL$USER", "LNM$SYSTEM_TABLE") .nes. "" then -
     deassign/system/executive_mode BESZEL$USER
$ exit 1
