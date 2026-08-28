#include "mruby_filter.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "mruby.h"
#include "mruby/array.h"
#include "mruby/compile.h"

#define TAG "MRBFILT"

// EMBED_TXTFILES (main/CMakeLists.txt) for mruby_scripts/default.rb - used
// whenever mrb_script (below) is empty/erased/invalid.
extern const uint8_t mruby_default_script_start[] asm("_binary_default_rb_start");
extern const uint8_t mruby_default_script_end[]   asm("_binary_default_rb_end");

// Raw storage (not a filesystem) for one uploaded script - see
// partitions.csv and bin/upload_mruby_script.py. Layout: 4-byte
// little-endian length, then that many bytes of UTF-8 source. This is
// what makes editing behavior not require a rebuild/reflash of the app
// image - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
#define MRB_SCRIPT_PARTITION_LABEL   "mrb_script"
#define MRB_SCRIPT_PARTITION_SUBTYPE 0x50

static mrb_state *s_mrb;
static bool s_active;
static char s_hostname[64];
static bool s_hostname_set;

static mrb_value ruby_hostname(mrb_state *mrb, mrb_value self)
{
    (void)self;
    const char *name;
    mrb_int len;
    mrb_get_args(mrb, "s", &name, &len);
    if (len < 0) {
        len = 0;
    }
    if ((size_t)len >= sizeof(s_hostname)) {
        len = sizeof(s_hostname) - 1;
    }
    memcpy(s_hostname, name, (size_t)len);
    s_hostname[len] = '\0';
    s_hostname_set = true;
    return mrb_nil_value();
}

// Returns true if a valid uploaded script was found and mrb_load_nstring()'d
// (caller must still check_error() - a script can be present but fail to
// parse). False means the partition is missing/erased/corrupt - caller
// should fall back to the embedded default.rb, not treat this as fatal.
static bool load_uploaded_script(mrb_state *mrb)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, MRB_SCRIPT_PARTITION_SUBTYPE, MRB_SCRIPT_PARTITION_LABEL);
    if (part == NULL) {
        return false;
    }

    uint32_t len;
    if (esp_partition_read(part, 0, &len, sizeof(len)) != ESP_OK) {
        return false;
    }
    if (len == 0 || len == 0xFFFFFFFFu || len > part->size - sizeof(len)) {
        return false; // never uploaded (erased flash reads as 0xFF) or corrupt
    }

    char *buf = malloc(len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "load_uploaded_script: malloc(%" PRIu32 ") failed", len);
        return false;
    }
    esp_err_t err = esp_partition_read(part, sizeof(len), buf, len);
    if (err != ESP_OK) {
        free(buf);
        return false;
    }

    mrb_load_nstring(mrb, buf, len);
    free(buf);
    return true;
}

// Every call site below runs this right after mrb_load_* /
// mrb_funcall_argv() - mruby doesn't raise a C exception, it just leaves
// mrb->exc set. Logging and clearing it here is what makes "fail open"
// mid-flight (mid-report, not just at boot) possible - see the failure
// handling in each mruby_filter_*/mruby_route_*_also_udp() below.
static bool check_error(void)
{
    if (s_mrb->exc) {
        mrb_print_error(s_mrb);
        s_mrb->exc = NULL;
        return true;
    }
    return false;
}

void mruby_filter_init(void)
{
#if !CONFIG_MRUBY_FILTER_ROUTE_ENABLE
    ESP_LOGI(TAG, "CONFIG_MRUBY_FILTER_ROUTE_ENABLE off - using C filter_rules.h/route_rules.h");
    return;
#else
    s_mrb = mrb_open();
    if (s_mrb == NULL) {
        ESP_LOGE(TAG, "mrb_open() failed, falling back to C filter_rules.h/route_rules.h");
        return;
    }

    mrb_define_method(s_mrb, s_mrb->kernel_module, "hostname", ruby_hostname, MRB_ARGS_REQ(1));

    bool loaded = false;
    if (load_uploaded_script(s_mrb)) {
        if (check_error()) {
            ESP_LOGW(TAG, "Uploaded mrb_script failed to load, falling back to embedded default.rb");
        } else {
            loaded = true;
            ESP_LOGI(TAG, "Loaded uploaded script from mrb_script partition (no reflash)");
        }
    }
    if (!loaded) {
        size_t len = (size_t)(mruby_default_script_end - mruby_default_script_start);
        mrb_load_nstring(s_mrb, (const char *)mruby_default_script_start, len);
        if (check_error()) {
            ESP_LOGE(TAG, "Embedded default.rb failed too, falling back to C filter_rules.h/route_rules.h");
            mrb_close(s_mrb);
            s_mrb = NULL;
            return;
        }
        ESP_LOGI(TAG, "Loaded embedded default.rb (no mrb_script uploaded)");
    }

    s_active = true;
    ESP_LOGI(TAG, "mruby VM active (hostname %s)", s_hostname_set ? s_hostname : "not set by script");
#endif
}

bool mruby_filter_active(void)
{
    return s_active;
}

const char *mruby_filter_hostname(void)
{
    return s_hostname_set ? s_hostname : NULL;
}

bool mruby_filter_keyboard_report(uint8_t *modifiers, uint8_t keycodes[6])
{
    // mrb_funcall_argv()'s return value (and any objects it allocates,
    // like the array literal a script's method body returns) get pinned
    // in the GC arena until this is restored - without it every call here
    // leaks, and this runs on every keyboard report. See
    // mds/usb_hid/2026-08-29_mruby_phase1_impl.md - this is what the
    // real-hardware "(unknown):0: Out of memory (NoMemoryError)" during
    // mouse movement (mruby_filter_mouse_report() below, far higher call
    // rate than keyboard) turned out to be.
    int ai = mrb_gc_arena_save(s_mrb);

    mrb_value kc = mrb_ary_new_capa(s_mrb, 6);
    for (int i = 0; i < 6; i++) {
        mrb_ary_push(s_mrb, kc, mrb_fixnum_value(keycodes[i]));
    }
    mrb_value args[2] = { mrb_fixnum_value(*modifiers), kc };
    mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                      mrb_intern_cstr(s_mrb, "filter_keyboard"), 2, args);
    if (check_error() || mrb_type(ret) != MRB_TT_ARRAY || RARRAY_LEN(ret) != 3) {
        ESP_LOGW(TAG, "filter_keyboard: script error or bad return shape, forwarding unmodified");
        mrb_gc_arena_restore(s_mrb, ai);
        return true; // fail open: forward as-is rather than silently drop input
    }

    *modifiers = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 0));
    mrb_value out_kc = mrb_ary_ref(s_mrb, ret, 1);
    mrb_int out_len = mrb_type(out_kc) == MRB_TT_ARRAY ? RARRAY_LEN(out_kc) : 0;
    for (int i = 0; i < 6; i++) {
        keycodes[i] = (i < out_len) ? (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, out_kc, i)) : 0;
    }
    bool forward = mrb_test(mrb_ary_ref(s_mrb, ret, 2));
    mrb_gc_arena_restore(s_mrb, ai);
    return forward;
}

bool mruby_filter_mouse_report(uint8_t *buttons, int16_t *dx, int16_t *dy,
                                int8_t *wheel, int8_t *pan,
                                uint8_t *synth_modifiers, uint8_t *synth_keycode)
{
    int ai = mrb_gc_arena_save(s_mrb); // see mruby_filter_keyboard_report() above

    mrb_value args[5] = {
        mrb_fixnum_value(*buttons), mrb_fixnum_value(*dx), mrb_fixnum_value(*dy),
        mrb_fixnum_value(*wheel), mrb_fixnum_value(*pan),
    };
    mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                      mrb_intern_cstr(s_mrb, "filter_mouse"), 5, args);
    if (check_error() || mrb_type(ret) != MRB_TT_ARRAY || RARRAY_LEN(ret) != 8) {
        ESP_LOGW(TAG, "filter_mouse: script error or bad return shape, forwarding unmodified");
        mrb_gc_arena_restore(s_mrb, ai);
        return true;
    }

    *buttons          = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 0));
    *dx               = (int16_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 1));
    *dy               = (int16_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 2));
    *wheel            = (int8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 3));
    *pan              = (int8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 4));
    *synth_modifiers  = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 5));
    *synth_keycode    = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 6));
    bool forward_typec = mrb_test(mrb_ary_ref(s_mrb, ret, 7));
    mrb_gc_arena_restore(s_mrb, ai);
    return forward_typec;
}

bool mruby_route_keyboard_also_udp(uint8_t modifiers, const uint8_t keycodes[6])
{
    int ai = mrb_gc_arena_save(s_mrb); // see mruby_filter_keyboard_report() above

    mrb_value kc = mrb_ary_new_capa(s_mrb, 6);
    for (int i = 0; i < 6; i++) {
        mrb_ary_push(s_mrb, kc, mrb_fixnum_value(keycodes[i]));
    }
    mrb_value args[2] = { mrb_fixnum_value(modifiers), kc };
    mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                      mrb_intern_cstr(s_mrb, "route_keyboard_udp"), 2, args);
    bool also_udp = false;
    if (!check_error()) {
        also_udp = mrb_test(ret); // fail closed for routing: don't spam an extra UDP mirror on script error
    }
    mrb_gc_arena_restore(s_mrb, ai);
    return also_udp;
}

bool mruby_route_mouse_also_udp(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    int ai = mrb_gc_arena_save(s_mrb); // see mruby_filter_keyboard_report() above

    mrb_value args[5] = {
        mrb_fixnum_value(buttons), mrb_fixnum_value(dx), mrb_fixnum_value(dy),
        mrb_fixnum_value(wheel), mrb_fixnum_value(pan),
    };
    mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                      mrb_intern_cstr(s_mrb, "route_mouse_udp"), 5, args);
    bool also_udp = false;
    if (!check_error()) {
        also_udp = mrb_test(ret);
    }
    mrb_gc_arena_restore(s_mrb, ai);
    return also_udp;
}

bool mruby_route_consumer_also_udp(uint16_t usage_id)
{
    int ai = mrb_gc_arena_save(s_mrb); // see mruby_filter_keyboard_report() above

    mrb_value args[1] = { mrb_fixnum_value(usage_id) };
    mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                      mrb_intern_cstr(s_mrb, "route_consumer_udp"), 1, args);
    bool also_udp = false;
    if (!check_error()) {
        also_udp = mrb_test(ret);
    }
    mrb_gc_arena_restore(s_mrb, ai);
    return also_udp;
}
