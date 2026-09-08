#ifndef MRUBY_ALLOC_PSRAM_H
#define MRUBY_ALLOC_PSRAM_H

#include <stdbool.h>

/**
 * Replaces mruby's own default mrb_basic_alloc_func() (mruby's
 * src/allocf.c - deliberately stripped out of libmruby.a, see
 * components/mruby/CMakeLists.txt's mruby_strip_allocf target) with one
 * that, while `enabled` is true, routes every mruby allocation through
 * heap_caps_realloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) instead of
 * plain realloc() (which this board's SPIRAM_USE_CAPS_ALLOC setting keeps
 * internal-SRAM-only - see sdkconfig.defaults). Falls back to plain
 * realloc() if the PSRAM allocation itself somehow fails, or whenever
 * `enabled` is false - so this is a complete no-op change for any
 * mrb_state that never touches this window.
 *
 * Global, not per-mrb_state: mruby has no per-instance allocator hook in
 * this version (mrb_open_core()/mrb_open() always resolve allocations to
 * the same global mrb_basic_alloc_func(), see mruby's src/state.c/gc.c -
 * there's no mrb_open_allocf() equivalent to attach a different allocator
 * to just one VM). This is meant to be toggled tightly around one
 * throwaway mrb_state's entire lifetime only (mruby_filter.c's
 * mruby_filter_check_syntax(), the only intended caller) - not left on
 * indefinitely: any *other* mrb_state's allocations (the real, always-
 * running s_mrb, possibly executing on a different task) happening to
 * land while this is enabled would also get redirected to PSRAM for that
 * instant. Harmless (PSRAM works fine functionally, this project already
 * uses it explicitly elsewhere - see mruby_webui.c's webui_alloc()) but
 * slower per-access than internal SRAM, so keep the enabled window as
 * short as possible around just the syntax-check call.
 *
 * See mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md for why this
 * exists: a throwaway mrb_state doing a full Prism parse+codegen pass can
 * need more contiguous internal SRAM than is left once BLE/NimBLE is
 * active, even though ~8MB of PSRAM sits free the entire time.
 */
void mruby_alloc_prefer_psram(bool enabled);

#endif
