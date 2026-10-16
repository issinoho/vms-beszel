$! MAKE_KIT.COM - build the beszel agent's PCSI kit for this node's architecture
$!
$! Usage:  @[.KIT]MAKE_KIT          (from the work directory, after @BUILD_VMS)
$! Needs [.BUILD]BESZEL_AGENT.EXE and BESZEL_LAUNCH.EXE.  The product
$! description, text module, KIT.ENV, README.VMS and CACERT.PEM were put in
$! [.KIT] by tools/kit.sh.  Writes the kit to [.KIT_<arch>].
$!
$ set noon
$ set process/parse_style=extended
$ status = 44
$ saved_default = f$environment("DEFAULT")
$ proc = f$environment("PROCEDURE")
$ set default 'f$parse(proc,,,"DEVICE")''f$parse(proc,,,"DIRECTORY")'
$ set default [-]
$ arch = f$edit(f$getsyi("ARCH_NAME"), "UPCASE")
$ base = "I64VMS"
$ if arch .eqs. "X86_64" then base = "X86VMS"
$!
$! KIT_PRODUCER, KIT_PRODUCT, PCSI_VERSION, KIT_VERSION from KIT.ENV
$ open/read env [.KIT]KIT.ENV
$env_loop:
$ read/end=env_done env line
$ name = f$element(0, "=", line)
$ 'name' = f$element(1, "=", line)
$ goto env_loop
$env_done:
$ close env
$!
$ if f$search("[.BUILD]BESZEL_AGENT.EXE") .eqs. "" .or. f$search("[.BUILD]BESZEL_LAUNCH.EXE") .eqs. ""
$ then
$   write sys$error "MAKE_KIT: no [.BUILD]BESZEL_AGENT.EXE or BESZEL_LAUNCH.EXE; @BUILD_VMS first"
$   goto done
$ endif
$! The image must report the version the kit claims.
$ agent = "$" + f$search("[.BUILD]BESZEL_AGENT.EXE")
$ define/user sys$output [.KIT]VERSION.TMP
$ agent "--version"
$ open/read v [.KIT]VERSION.TMP
$ read v got
$ close v
$ delete/nolog [.KIT]VERSION.TMP;*
$ if got .nes. "beszel_agent " + KIT_VERSION
$ then
$   write sys$error "MAKE_KIT: BESZEL_AGENT.EXE says """, got, """, the kit is ", KIT_VERSION, "; rebuild"
$   goto done
$ endif
$!
$! Gather the files flat in [.KIT_<arch>.MAT]: PRODUCT PACKAGE looks each one up
$! by name in the material directory (destinations are in the PDF).
$ mat = "[.KIT_''arch'.MAT]"
$ out = "[.KIT_''arch']"
$ if f$search("KIT_''arch'.DIR") .eqs. "" then create/directory 'out'
$ if f$search("[.KIT_''arch']MAT.DIR") .eqs. "" then create/directory 'mat'
$ if f$search("''out'*.PCSI;*") .nes. "" then delete/nolog 'out'*.PCSI;*
$ if f$search("''mat'*.*;*") .nes. "" then delete/nolog 'mat'*.*;*
$ copy/nolog [.BUILD]BESZEL_AGENT.EXE,BESZEL_LAUNCH.EXE 'mat'
$ copy/nolog [.VMS]BESZEL$CONTROL.COM,BESZEL$RUN.COM 'mat'
$ copy/nolog [.KIT]BESZEL$CONFIGURE.COM,BESZEL$STARTUP.COM,BESZEL$SHUTDOWN.COM 'mat'
$ copy/nolog [.VMS]AGENT.CONF_TEMPLATE 'mat'AGENT.CONF_TEMPLATE
$ copy/nolog [.KIT]CACERT.PEM,README.VMS 'mat'
$ copy/nolog []LICENSE. 'mat'LICENSE.
$ matspec = f$parse(mat,,,"DEVICE","NO_CONCEAL") + f$parse(mat,,,"DIRECTORY","NO_CONCEAL")
$!
$ write sys$output "MAKE_KIT: ''KIT_PRODUCER' ''base' ''KIT_PRODUCT' ''PCSI_VERSION' (beszel agent ''KIT_VERSION')"
$ product package 'KIT_PRODUCT' -
    /producer='KIT_PRODUCER' /base_system='base' /version='PCSI_VERSION' -
    /source=[.KIT]PRODUCT-'base'.PCSI$DESC -
    /material='matspec' -
    /destination='out' -
    /format=sequential -
    /options=noconfirm /log
$ status = $status
$ kit = f$search("''out'*.PCSI")
$ if kit .nes. "" then write sys$output "MAKE_KIT: kit ", kit
$ if kit .eqs. "" then status = 44
$done:
$ set default 'saved_default'
$ exit status
