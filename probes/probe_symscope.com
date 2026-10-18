$! does a global symbol set by a nested procedure override a local default?
$! (NOTES.md 46)
$ set noon
$ open/write f sys$scratch:probe_symscope_cfg.com
$ write f "$ beszel_autostart == ""YES"""
$ close f
$ beszel_autostart = "NO"
$ @sys$scratch:probe_symscope_cfg.com
$ write sys$output "with local default: ", beszel_autostart
$ delete/symbol/local beszel_autostart
$ write sys$output "after deleting local: ", beszel_autostart
$ delete/nolog sys$scratch:probe_symscope_cfg.com;*
$ delete/symbol/global beszel_autostart
