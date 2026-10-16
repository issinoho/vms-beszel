$! BESZEL$CONFIGURE.COM - set up the beszel agent (run once, as SYSTEM; safe to run again)
$!
$! Usage:  @BESZEL$ROOT:[COM]BESZEL$CONFIGURE [account] [uic] [autostart]
$!   account    the agent's account (default BESZEL)
$!   uic        its UIC, e.g. [377,1]: a group of its own, above MAXSYSGROUP.
$!              Needed only when the account is new.
$!   autostart  YES: BESZEL$STARTUP starts the agent at boot (default YES)
$! Missing values are asked for when run interactively.
$!
$! What it does (README.VMS; DECISIONS.md D12, D14):
$!  - the account.  A new one gets NETMBX and TMPMBX only, no login class at all
$!    (local, dial-up, remote, network, batch), no DISUSER flag (OpenVMS won't
$!    start processes as a disusered account), LGICMD NL:, DISCTLY and LOCKPWD,
$!    and a random password nobody needs.  An existing account is only changed
$!    if this procedure created it (its owner field says "beszel agent"); its
$!    privileges and logins are then set as above again.  A system-group UIC is
$!    refused.
$!  - the tree: [CONF], [DATA] and [LOG] belong to the account and only it can
$!    read them; [BIN], [COM], [DOC] are readable by all.
$!  - BESZEL$ROOT:[CONF]AGENT.CONF from AGENT.CONF_TEMPLATE, if there is none.
$!  - SYS$MANAGER:BESZEL$CONFIG.COM (the account; start at boot or not), and the
$!    system logical name BESZEL$USER.
$!
$ set noon
$ set process/parse_style=extended
$ say = "write sys$output"
$ interactive = f$mode() .eqs. "INTERACTIVE"
$ if f$trnlnm("BESZEL$ROOT", "LNM$SYSTEM_TABLE") .eqs. ""
$ then
$   say "BESZEL$CONFIGURE: BESZEL$ROOT is not defined (@SYS$STARTUP:BESZEL$STARTUP)"
$   exit 44
$ endif
$ if .not. f$privilege("SYSPRV,CMKRNL")
$ then
$   say "BESZEL$CONFIGURE: needs SYSPRV and CMKRNL (run it as SYSTEM)"
$   exit 36
$ endif
$!
$ beszel_account == ""
$ beszel_autostart == ""
$ if f$search("SYS$MANAGER:BESZEL$CONFIG.COM") .nes. "" then @SYS$MANAGER:BESZEL$CONFIG.COM
$ accdef = "BESZEL"
$ if beszel_account .nes. "" then accdef = beszel_account
$ autodef = "YES"
$ if beszel_autostart .nes. "" then autodef = beszel_autostart
$ account = f$edit(p1, "UPCASE,TRIM")
$ if account .eqs. "" .and. interactive then inquire account "The agent's account [''accdef']"
$ account = f$edit(account, "UPCASE,TRIM")
$ if account .eqs. "" then account = accdef
$ maxsys = f$getsyi("MAXSYSGROUP")
$ uicnum = f$identifier(account, "NAME_TO_NUMBER")
$!
$ ! a work file for AUTHORIZE: in [CONF], which only SYSTEM and the account can read
$ work = "BESZEL$ROOT:[CONF]BESZEL$CONFIGURE.TMP"
$ uaf = f$parse("SYS$SYSTEM:SYSUAF.DAT")
$ if uicnum .eq. 0 then goto new_account
$!
$! ---- an existing account: change it only if we made it ----
$ if uicnum / 65536 .le. maxsys
$ then
$   say "BESZEL$CONFIGURE: ''account' has a system UIC; the agent must have its own account (D12)"
$   exit 44
$ endif
$ define/user sys$output 'work'
$ define/user sysuaf 'uaf'
$ mcr authorize show 'account'
$ owner = ""
$ open/read f 'work'
$read_owner:
$ read/end=owner_done f line
$ if f$locate("Owner:", line) .lt. f$length(line) then -
     owner = f$edit(f$extract(f$locate("Owner:", line) + 6, 999, line), "TRIM,COMPRESS")
$ goto read_owner
$owner_done:
$ close f
$ delete/nolog 'work';*
$ if f$edit(owner, "UPCASE") .nes. "BESZEL AGENT"
$ then
$   say "BESZEL$CONFIGURE: an account ''account' exists, but this procedure didn't create it"
$   say "  (its owner field is """, owner, """). Choose another name, so a real user's"
$   say "  account isn't changed."
$   exit 44
$ endif
$ say "Account ''account' exists (", f$fao("!%U", uicnum), "); setting its privileges and logins again"
$ open/write f 'work'
$ write f "modify ''account' /privileges=(netmbx,tmpmbx) /defprivileges=(netmbx,tmpmbx) -"
$ write f "  /flags=(nodisuser,disctly,lockpwd,norestricted) /lgicmd=""NL:"" -"
$ write f "  /nolocal /nodialup /noremote /nonetwork /nobatch"
$ write f "exit"
$ close f
$ goto run_authorize
$!
$! ---- a new account ----
$new_account:
$ uic = f$edit(p2, "UPCASE,TRIM")
$ if uic .eqs. "" .and. interactive then -
     inquire uic "UIC for ''account', a group of its own above ''maxsys' (e.g. [377,1])"
$ uic = f$edit(uic, "UPCASE,TRIM")
$ if uic .eqs. "" .or. f$extract(0, 1, uic) .nes. "[" .or. f$locate(",", uic) .eq. f$length(uic)
$ then
$   say "BESZEL$CONFIGURE: ''account' is a new account: give its UIC, e.g. [377,1]"
$   exit 44
$ endif
$ grp = f$integer("%O" + (f$element(0, ",", uic) - "["))
$ if grp .le. maxsys
$ then
$   say "BESZEL$CONFIGURE: group ", f$fao("!OL", grp), " is a system group (MAXSYSGROUP is ", maxsys, -
        "): choose a group above it (D12)"
$   exit 44
$ endif
$ ! a password nobody needs: 32 random characters from OpenSSL's generator
$ agent := $BESZEL$ROOT:[BIN]BESZEL_AGENT.EXE
$ define/user sys$output 'work'
$ agent --random
$ open/read f 'work'
$ read f password
$ close f
$ delete/erase/nolog 'work';*
$ if f$length(password) .ne. 32
$ then
$   say "BESZEL$CONFIGURE: couldn't make a password (BESZEL_AGENT.EXE --random)"
$   exit 44
$ endif
$ datadev = f$parse("BESZEL$ROOT:[DATA]",,,"DEVICE","NO_CONCEAL")
$ datadir = f$parse("BESZEL$ROOT:[DATA]",,,"DIRECTORY","NO_CONCEAL") - "][" - "><"
$ say "Creating account ''account' ''uic'"
$ open/write f 'work'
$ write f "add ''account' /uic=''uic' /password=''password' /owner=""beszel agent"" -"
$ write f "  /privileges=(netmbx,tmpmbx) /defprivileges=(netmbx,tmpmbx) -"
$ write f "  /flags=(nodisuser,disctly,lockpwd) /lgicmd=""NL:"" /pwdlifetime=none -"
$ write f "  /nolocal /nodialup /noremote /nonetwork /nobatch -"
$ write f "  /device=''datadev' /directory=''datadir'"
$ write f "exit"
$ close f
$ password = ""
$run_authorize:
$ set file/protection=(s:rwd,o,g,w) 'work'
$ define/user sysuaf 'uaf'
$ define/user sys$input 'work'
$ mcr authorize
$ delete/erase/nolog 'work';*
$ uicnum = f$identifier(account, "NAME_TO_NUMBER")
$ if uicnum .eq. 0
$ then
$   say "BESZEL$CONFIGURE: AUTHORIZE didn't create ''account' (see above)"
$   exit 44
$ endif
$!
$! ---- the tree ----
$ rootdev = f$parse("BESZEL$ROOT:[000000]",,,"DEVICE","NO_CONCEAL")
$ rootdir = f$parse("BESZEL$ROOT:[000000]",,,"DIRECTORY","NO_CONCEAL") - "][" - "><" - ".000000"
$ ! the root's own directory file, e.g. SYS$COMMON:[VMS$COMMON]BESZEL.DIR:
$ ! split "[A.B.C]" at its last dot into "[A.B]" and "C"
$ inner = rootdir - "[" - "]"
$ n = 0
$count_dots:
$ if f$element(n + 1, ".", inner) .eqs. "." then goto counted
$ n = n + 1
$ goto count_dots
$counted:
$ last = f$element(n, ".", inner)
$ parent = rootdev + "[000000]"
$ if n .gt. 0 then parent = rootdev + "[" + f$extract(0, f$length(inner) - f$length(last) - 1, inner) + "]"
$ set file/protection=(s:rwe,o:rwe,g:re,w:re) 'parent''last'.DIR
$ set file/protection=(s:rwe,o:rwe,g:re,w:re) BESZEL$ROOT:[000000]BIN.DIR,COM.DIR,DOC.DIR
$ set file/protection=(s:rwed,o:rwed,g:re,w:re) BESZEL$ROOT:[BIN]*.*;*,[COM]*.*;*,[DOC]*.*;*
$ set file/owner='account'/protection=(s:rwe,o:rwe,g,w) BESZEL$ROOT:[000000]CONF.DIR,DATA.DIR,LOG.DIR
$ if f$search("BESZEL$ROOT:[CONF]AGENT.CONF") .eqs. ""
$ then
$   copy/nolog BESZEL$ROOT:[CONF]AGENT.CONF_TEMPLATE BESZEL$ROOT:[CONF]AGENT.CONF
$   say "Created BESZEL$ROOT:[CONF]AGENT.CONF from its template"
$ endif
$ if f$search("BESZEL$ROOT:[CONF]*.*;*") .nes. "" then set file/owner='account' BESZEL$ROOT:[CONF]*.*;*
$ if f$search("BESZEL$ROOT:[DATA]*.*;*") .nes. "" then set file/owner='account' BESZEL$ROOT:[DATA]*.*;*
$ if f$search("BESZEL$ROOT:[LOG]*.*;*") .nes. "" then set file/owner='account' BESZEL$ROOT:[LOG]*.*;*
$ ! AGENT.CONF holds the token: its owner only, every version
$ set file/protection=(s:rwed,o:rwed,g,w) BESZEL$ROOT:[CONF]AGENT.CONF;*
$ set file/protection=(s:rwed,o:rwed,g:r,w:r) BESZEL$ROOT:[CONF]CACERT.PEM;*,AGENT.CONF_TEMPLATE;*
$!
$! ---- site settings ----
$ autostart = f$edit(p3, "UPCASE,TRIM")
$ if autostart .eqs. "" .and. interactive then inquire autostart "Start the agent at boot (YES/NO) [''autodef']"
$ autostart = f$edit(autostart, "UPCASE,TRIM")
$ if autostart .eqs. "" then autostart = autodef
$ if autostart .nes. "YES" then autostart = "NO"
$ open/write f SYS$MANAGER:BESZEL$CONFIG.COM
$ write f "$! BESZEL$CONFIG.COM - site settings for the beszel agent, written by"
$ write f "$! BESZEL$ROOT:[COM]BESZEL$CONFIGURE.COM (run it again to change them)"
$ write f "$ beszel_account == ""''account'"""
$ write f "$ beszel_autostart == ""''autostart'"""
$ close f
$ set file/protection=(s:rwed,o:rwed,g:re,w) SYS$MANAGER:BESZEL$CONFIG.COM
$ define/system/executive_mode BESZEL$USER 'account'
$!
$ say ""
$ say "The beszel agent will run as ''account' ", f$fao("!%U", uicnum), -
      " (start at boot: ''autostart')."
$ say "Next:"
$ say "  1. Put the hub's address, key and token (from its ""Add System"" dialog) in"
$ say "     BESZEL$ROOT:[CONF]AGENT.CONF"
$ say "  2. $ @BESZEL$ROOT:[COM]BESZEL$CONTROL START"
$ say "  3. Add $ @SYS$STARTUP:BESZEL$STARTUP.COM to SYS$MANAGER:SYSTARTUP_VMS.COM (after"
$ say "     TCP/IP) and $ @SYS$STARTUP:BESZEL$SHUTDOWN.COM to SYS$MANAGER:SYSHUTDWN.COM"
$ exit 1
