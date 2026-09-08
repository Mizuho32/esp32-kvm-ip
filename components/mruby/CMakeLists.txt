# mruby is not built through ESP-IDF's own compiler invocations - it has
# its own rake-based build system (gem_init.c generation, presym scanning,
# .rb -> bytecode precompilation via a host-built mrbc). Vendoring its
# generated files by hand and hoping they stay in sync with whichever gems
# esp32s3_build_config.rb selects is more fragile than just shelling out to
# `rake` as a custom build step and linking the resulting static library -
# see mds/usb_hid/2026-08-28_mruby_filter_route.md's Phase1 implementation
# notes. This requires Ruby (+ rake) on the machine running `idf.py build`,
# in addition to the ESP-IDF toolchain.

# mruby/build/esp32s3/include holds generated headers (mruby/presym/id.h,
# mruby/presym/table.h) produced by the rake build below - not just static
# sources under mruby/include.
idf_component_register(INCLUDE_DIRS "mruby/include" "mruby/build/esp32s3/include")

set(MRUBY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/mruby")
set(MRUBY_BUILD_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/esp32s3_build_config.rb")
set(MRUBY_LIB "${MRUBY_ROOT}/build/esp32s3/lib/libmruby.a")

find_program(RAKE_EXECUTABLE rake)
if(NOT RAKE_EXECUTABLE)
    message(FATAL_ERROR "mruby component: `rake` not found on PATH. mruby's own build "
                         "system needs a Ruby + rake install on the machine running "
                         "idf.py build (independent of the ESP-IDF toolchain) - see "
                         "components/mruby/CMakeLists.txt.")
endif()

# rake does its own internal up-to-date checking (mtimes), so this is safe
# to invoke unconditionally on every build - it's a fast no-op once
# libmruby.a is current. Avoids having to keep a CMake-side source file
# list in sync with esp32s3_build_config.rb's gem selection.
add_custom_target(mruby_build
    COMMAND ${CMAKE_COMMAND} -E env "MRUBY_CONFIG=${MRUBY_BUILD_CONFIG}"
            ${RAKE_EXECUTABLE} -j4
    WORKING_DIRECTORY "${MRUBY_ROOT}"
    BYPRODUCTS "${MRUBY_LIB}"
    COMMENT "Building mruby for esp32s3 (rake, see components/mruby/esp32s3_build_config.rb)"
    VERBATIM
)

# main/mruby_alloc_psram.c (mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md)
# provides its own mrb_basic_alloc_func() so the WebUI's throwaway
# syntax-check VM can be routed to PSRAM - but libmruby.a already defines
# that same symbol (src/allocf.c), so linking main/mruby_alloc_psram.c as
# well would be a duplicate-symbol error. mruby's own build.rb (invoked
# via esp32s3_build_config.rb, rake-driven - see the comment at the top of
# this file) gathers core src/*.c files internally with no per-file
# exclude hook exposed to us, and this component's mruby/ checkout stays
# an unmodified upstream submodule (not something to patch directly) - so
# strip just that one already-built object out of the finished archive
# instead. `ar d` on a member that's already gone (e.g. a rebuild that
# didn't need to regenerate allocf.o) is a harmless no-op (exit 0,
# confirmed), so this is safe to run unconditionally on every build, same
# as mruby_build's own rake invocation above.
add_custom_target(mruby_strip_allocf
    COMMAND ${CMAKE_AR} d "${MRUBY_LIB}" allocf.o
    DEPENDS mruby_build
    COMMENT "Stripping mruby's default allocf.o from libmruby.a (see main/mruby_alloc_psram.c)"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} mruby_strip_allocf)
target_link_libraries(${COMPONENT_LIB} INTERFACE "${MRUBY_LIB}")

# mruby is the first thing in this project to actually call longjmp()
# (its exception handling - src/error.c, vm.c, gc.c). Xtensa globally
# wraps longjmp (-Wl,--wrap=longjmp, see esp_rom/CMakeLists.txt) so a
# FreeRTOS context switch mid-longjmp can't corrupt the register windows -
# but static-archive link order means __wrap_longjmp's defining object
# (esp_rom's patches/esp_rom_longjmp.S) only gets pulled out of esp_rom.a
# if something already needs it *before* the linker moves past that
# archive. libmruby.a linking after esp_rom.a is too late, so force it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_longjmp")
