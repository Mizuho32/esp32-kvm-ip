# Custom cross-build config for embedding mruby into esp32-kvm-ip
# (KVM_ROLE=HOST filter/conv/route scripting - see
# mds/usb_hid/2026-08-28_mruby_filter_route.md). Not part of the mruby
# submodule itself (that stays an unmodified upstream checkout) - this
# file is invoked via MRUBY_CONFIG=<absolute path to this file> from
# CMakeLists.txt, so it lives here instead.
#
# mruby-socket is deliberately excluded from every gembox below: this VM
# never does its own network I/O (that stays in hid_forwarder.c/C), so
# mruby-socket's sys/socket.h dependency (which doesn't exist for this
# freestanding xtensa-esp32s3-elf target) is simply never pulled in.
#
# bigint/complex/rational/regexp/set/time/random/pack and the various
# "bin" mrbgems (mrbc/mirb/mruby CLI executables, irrelevant on-device)
# are excluded too - see the gembox breakdown in the design doc for why
# (roughly 150KB+ of gems this filter/route DSL has no use for).

# The 'host' build only exists to produce the on-build-machine mrbc tool
# mrbgems' .rb (mrblib) sources get precompiled with - it must use the
# *native* compiler, not the ESP32 cross toolchain, regardless of any
# ambient CC env var this is invoked under.
MRuby::Build.new do |conf|
  conf.toolchain
  conf.gembox 'stdlib'
  conf.gembox 'stdlib-ext'
  conf.gembox 'math'
  conf.gembox 'metaprog'
end

MRuby::CrossBuild.new('esp32s3') do |conf|
  toolchain :gcc

  # Hardcoded (not ENV['CC']) so this cross build is unaffected by
  # whatever CC the invoking build system happens to have set for the
  # 'host' build above - see mds/usb_hid/2026-08-28_mruby_filter_route.md's
  # Phase1 implementation notes for how this bit us during the feasibility
  # spike.
  conf.cc.command = 'xtensa-esp32s3-elf-gcc'
  conf.cc.flags << '-mlongcalls'
  conf.cxx.command = 'xtensa-esp32s3-elf-g++'
  conf.linker.command = 'xtensa-esp32s3-elf-gcc'
  conf.archiver.command = 'xtensa-esp32s3-elf-ar'

  conf.gembox 'stdlib'
  conf.gembox 'stdlib-ext'
  conf.gembox 'math'
  conf.gembox 'metaprog'

  # We only need libmruby.a (linked into esp32-kvm-ip's own app image) -
  # not a standalone mruby/mrbc/mirb executable, which wouldn't link
  # anyway without ESP-IDF's own linker script/startup files.
  conf.build_mrbtest_lib_only
end
