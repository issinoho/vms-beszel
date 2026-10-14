$! build_vms.com - build vms-beszel with VSI C (DECISIONS.md D2).
$!
$!   @build_vms [TEST]
$!
$! Phase 1: the portable core, the OpenSSL crypto and the unit tests; TEST runs
$! them.  (The VMS platform layer and the agent image come in Phase 2.)
$! Compile qualifiers follow vms-steward/vmsguard: C99, 32-bit pointers,
$! PREFIX=ALL_ENTRIES so the socket calls link, BSD 4.4 sockaddr.
$ set noon
$ say = "write sys$output"
$ define/nolog sys$error sys$output
$ here = f$parse(f$environment("procedure"),,,"device") + f$parse(f$environment("procedure"),,,"directory")
$ set default 'here'
$ if f$search("[]BUILD.DIR") .eqs. "" then create/directory [.build]
$ if f$trnlnm("SSL3$INCLUDE") .eqs. ""
$ then
$   say "BUILD-FAIL: SSL3$INCLUDE is not defined (VSI OpenSSL 3 kit)"
$   exit 44
$ endif
$ define/process/nolog OPENSSL SSL3$INCLUDE:
$ open/write o [.build]ssl.opt
$ write o "SYS$LIBRARY:SSL3$LIBSSL_SHR32/SHARE"
$ write o "SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32/SHARE"
$ close o
$ ccq = "/STANDARD=C99/POINTER_SIZE=32/PREFIX_LIBRARY_ENTRIES=ALL_ENTRIES" + -
        "/FLOAT=IEEE_FLOAT/IEEE_MODE=DENORM_RESULTS" + -
        "/DEFINE=(_SOCKADDR_LEN)/NOLIST" + -
        "/INCLUDE_DIRECTORY=([.src.core],[.src.plat])"
$ failed = 0
$ sources = "[.src.core]bz_buf,[.src.core]bz_cbor,[.src.core]bz_text,[.src.core]bz_conf," + -
            "[.src.core]bz_key,[.src.core]bz_url,[.src.core]bz_ws,[.src.core]bz_proto," + -
            "[.src.core]bz_stats,[.src.core]bz_agent,[.src.plat]bz_crypto_ossl," + -
            "[.tests.unit]test_core"
$ i = 0
$cc_loop:
$ s = f$element(i, ",", sources)
$ if s .eqs. "," then goto cc_done
$ i = i + 1
$ name = f$parse(s,,,"NAME","SYNTAX_ONLY")
$ cc'ccq' 's'.c /object=[.build]'name'.obj
$ sev = $severity
$ if sev .ne. 1 .and. sev .ne. 3
$ then
$   say "BUILD-FAIL compile ", s, " (severity ", sev, ")"
$   failed = failed + 1
$ endif
$ goto cc_loop
$cc_done:
$ if failed .gt. 0
$ then
$   say "BUILD-FAIL: ", failed, " compile(s) failed"
$   exit 44
$ endif
$ core = "[.build]bz_buf,[.build]bz_cbor,[.build]bz_text,[.build]bz_conf,[.build]bz_key," + -
         "[.build]bz_url,[.build]bz_ws,[.build]bz_proto,[.build]bz_stats,[.build]bz_agent," + -
         "[.build]bz_crypto_ossl"
$! Undefined symbols are only warnings to LINK: check the severity.
$ link/executable=[.build]test_core.exe 'core',[.build]test_core,[.build]ssl.opt/options
$ if $severity .ne. 1
$ then
$   say "BUILD-FAIL: link test_core"
$   exit 44
$ endif
$ say "BUILD-OK"
$ if f$edit(p1, "UPCASE") .eqs. "TEST"
$ then
$   run [.build]test_core
$ endif
$ exit
