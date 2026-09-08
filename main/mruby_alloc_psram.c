// Replacement for mruby's own src/allocf.c (deliberately stripped out of
// libmruby.a - see components/mruby/CMakeLists.txt's mruby_strip_allocf
// custom target and mruby_alloc_psram.h). mruby's src/allocf.c documents
// this exact override as a supported customization point ("If you want
// to use your own memory allocator ... redefine mrb_basic_alloc_func()
// in your application") - this is that redefinition, not a patch to the
// vendored mruby submodule itself (which stays an unmodified upstream
// checkout, per components/mruby/CMakeLists.txt's own comment).

#include "mruby_alloc_psram.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "mruby.h" // declares mrb_basic_alloc_func()'s exact prototype

static volatile bool s_prefer_psram;

void mruby_alloc_prefer_psram(bool enabled)
{
    s_prefer_psram = enabled;
}

// Signature/semantics must exactly match mruby's original (src/allocf.c):
// size == 0 frees and returns NULL; otherwise this acts like
// realloc(p, size) (p may be NULL, same as malloc(size)). free()/
// realloc() are heap-region-agnostic in ESP-IDF - both ultimately resolve
// to heap_caps_free()/heap_caps_realloc_default() (see
// components/esp_libc/src/heap.c), which auto-detect which registered
// heap a given pointer already belongs to - so a plain free() below
// correctly frees a PSRAM-origin block too. Only the *allocating* path
// needs an explicit heap_caps_realloc() call to actually land in PSRAM:
// this board's SPIRAM_USE_CAPS_ALLOC setting (sdkconfig.defaults) keeps
// plain malloc()/realloc() internal-SRAM-only by design (mruby's own VM
// heap and the latency-sensitive mouse dispatch path are untouched by
// that setting), so realloc() alone would never reach PSRAM on its own.
void *mrb_basic_alloc_func(void *p, size_t size)
{
    if (size == 0) {
        free(p);
        return NULL;
    }
    if (s_prefer_psram) {
        void *r = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (r != NULL) {
            return r;
        }
        // Fall through to the internal-SRAM path below - should never
        // actually happen in practice (this board has ~8MB of PSRAM,
        // essentially always free - see mds/usb_hid/2026-09-09_ble_webui_syntax_check_oom.md),
        // but matches the original allocf.c's own single-attempt
        // behavior (no retry loop) rather than returning NULL outright.
    }
    return realloc(p, size);
}
