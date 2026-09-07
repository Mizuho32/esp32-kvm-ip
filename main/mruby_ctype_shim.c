// Host role only (KVM_ROLE=HOST). Link-time shim, not a real ctype table.
//
// mruby's mruby-sprintf gem (always embedded - part of the default
// gembox, see mruby_scripts/default.rb's mrbgems list at boot) calls
// isspace()/isdigit()/etc. from sprintf.c. mruby is built via its own
// Rakefile-driven cross-compile step (separate from ESP-IDF's own CFLAGS/
// feature-test macros), which apparently resolves this toolchain's
// ctype.h to the classic table-lookup form (`_ctype_[c+1] & MASK`)
// instead of the function-call form ESP-IDF's own C files get - and this
// exact xtensa-esp-elf toolchain's own libc doesn't actually define
// `_ctype_` at all (confirmed: absent from every linked archive,
// including the toolchain's own libc.a - a real gap, not a link-order
// issue like __getreent's, which mruby's Prism parser needed and *is*
// available - see main/CMakeLists.txt's `-Wl,-u,__getreent`).
//
// No script this project runs calls Kernel#sprintf/format with anything
// ctype-sensitive (this project's own mruby DSL never calls it at all -
// this reference only exists because the gem is always linked in,
// unconditionally reachable via mrbgems' init table), so an all-zero
// stand-in table (every classification bit unset) is functionally
// harmless here - it exists purely to satisfy the linker. If a script
// ever actually needs correct sprintf()-via-ctype behavior (e.g. %s
// field-width padding with locale-sensitive classification), this needs
// replacing with mruby's build actually routing through the same ctype.h
// path as the rest of the firmware instead.
const unsigned char _ctype_[384] = {0};

// mruby_ctype_shim_touch(): a real, called-from-somewhere-early function
// (see mruby_filter.c's mruby_filter_init(), which calls this
// unconditionally) - exists purely so *something* has an actual
// (non-force-flag) reason to pull this translation unit's object out of
// libmain.a during the link's normal, early left-to-right archive scan.
//
// Without this, `_ctype_` above sits in the archive completely unwanted
// until libmruby.a (linked dead-last, see main/CMakeLists.txt's comment
// on __getreent) is scanned and *first* discovers it wants `_ctype_` -
// by which point ld has already finished scanning every earlier archive
// (including libmain.a, even the --whole-archive-forced instance of it,
// which is scanned then too - archive scanning is one single
// left-to-right pass, it never goes back to satisfy a need discovered
// later). Empirically confirmed via a manual `-Wl,--start-group
// ... --end-group` wrap around the *entire* link command, which forces
// ld to keep re-scanning until nothing new resolves - that fixed it,
// proving this really is an ordering problem, not a missing/wrong
// symbol. Getting main's own build to actually reference this function
// achieves the same effect in the normal, single-pass case, without
// needing to hand-wrap ESP-IDF's own generated (and not easily
// hookable) executable link command.
void mruby_ctype_shim_touch(void)
{
}
