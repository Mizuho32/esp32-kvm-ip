#include "mruby_filter.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mruby.h"
#include "mruby/array.h"
#include "mruby/compile.h"
#include "mruby/hash.h"
#include "mruby/string.h"

#include "hid_forwarder.h"
#include "usb_device_typec.h"

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

// ---- DSL data model: source/sink/pipeline registries -----------------
//
// See mds/usb_hid/2026-08-28_mruby_filter_route.md's "3. Src/Sinkの抽象化"
// section for the design, and mds/usb_hid/2026-08-29_mruby_phase1_impl.md
// for what this implementation deliberately simplifies (Hash-based event
// objects, mouse_synth_keys as a separate hook, no :udp source).
//
// Everything below is resolved exactly once, while the script's top-level
// source/sink/pipeline/from/to/branch calls execute during mrb_load_nstring()
// - not re-walked per HID report. hid_forwarder.c's per-report calls
// (mruby_dispatch_keyboard/mouse/consumer) only ever index into these
// already-built fixed-size arrays and invoke the handful of stored Proc
// blocks relevant to that one kind - see the design doc's "実行時の性能設計"
// note this is meant to satisfy.

#define MRB_DSL_MAX_SINKS  8
#define MRB_DSL_MAX_STAGES 6

enum { PIPE_KEYBOARD, PIPE_MOUSE, PIPE_CONSUMER, PIPE_KIND_COUNT };

typedef enum { SINK_TYPEC, SINK_UDP } sink_kind_t;

typedef struct {
    mrb_sym name;
    sink_kind_t kind;
    int event_kind_hint;          // -1 = none given; else PIPE_* (sink's own `kind:` opt, validated against whichever pipeline references it)
    char udp_host[64];            // valid when kind == SINK_UDP
    int udp_port;
    bool udp_resolved;            // getaddrinfo() is deferred - see mruby_filter_resolve_udp_sinks()
    struct sockaddr_in udp_addr;  // only meaningful once udp_resolved
} sink_def_t;

typedef struct {
    sink_def_t *sink;
    mrb_value block; // mrb_nil_value() if `to` was given no block (pure passthrough fan-out)
} to_stage_t;

typedef struct {
    sink_def_t *sink;
    mrb_value block; // always a Proc - required
} branch_stage_t;

typedef struct {
    to_stage_t to[MRB_DSL_MAX_STAGES];
    int to_count;
    branch_stage_t branch[MRB_DSL_MAX_STAGES];
    int branch_count;
} pipeline_t;

typedef struct {
    mrb_sym name;
    int kind; // PIPE_*
} source_def_t;

static sink_def_t s_sinks[MRB_DSL_MAX_SINKS];
static int s_sink_count;
static source_def_t s_sources[MRB_DSL_MAX_SINKS];
static int s_source_count;
static pipeline_t s_pipelines[PIPE_KIND_COUNT];

// Set by from() while inside a pipeline {...} block; to()/branch() append
// to s_pipelines[s_building_kind] while this is true. Pipelines never
// nest, so no stack is needed - see dsl_pipeline().
static bool s_building;
static int s_building_kind;

static mrb_state *s_mrb;
static bool s_active;
static char s_hostname[64];
static bool s_hostname_set;

static void reset_dsl_state(void)
{
    s_sink_count = 0;
    s_source_count = 0;
    memset(s_pipelines, 0, sizeof(s_pipelines)); // only zeroes to_count/branch_count meaningfully - see reset_dsl_state()'s call sites
    s_building = false;
    s_building_kind = 0;
    s_hostname_set = false;
}

// ---- small mruby helpers ----------------------------------------------

static mrb_value dsl_hkey(mrb_state *mrb, const char *name)
{
    return mrb_symbol_value(mrb_intern_cstr(mrb, name));
}

static void dsl_hset_int(mrb_state *mrb, mrb_value h, const char *key, mrb_int v)
{
    mrb_hash_set(mrb, h, dsl_hkey(mrb, key), mrb_fixnum_value(v));
}

static mrb_int dsl_hget_int(mrb_state *mrb, mrb_value h, const char *key, mrb_int fallback)
{
    mrb_value v = mrb_hash_get(mrb, h, dsl_hkey(mrb, key));
    return mrb_nil_p(v) ? fallback : mrb_fixnum(v);
}

static mrb_value dsl_opt(mrb_state *mrb, mrb_value opts, const char *key)
{
    if (mrb_nil_p(opts)) {
        return mrb_nil_value();
    }
    return mrb_hash_get(mrb, opts, dsl_hkey(mrb, key));
}

// Invokes a stored Proc via its #call method rather than mrb_yield_argv().
// mrb_funcall_argv() is a protected top-level entry point (an exception
// raised anywhere inside unwinds back to it, setting mrb->exc, rather than
// propagating further via longjmp) - this is what already made
// mrb_funcall_argv() safe to call from hid_forwarder.c's plain FreeRTOS
// task context in Phase1 (mds/usb_hid/2026-08-29_mruby_phase1_impl.md).
// mrb_yield_argv() does not carry that same guarantee when called from a
// context with no enclosing protected frame - as is the case for every
// mruby_dispatch_*() call below (invoked straight from a USB Host
// backend's task, never nested inside another mruby call), so it's
// deliberately avoided here.
static mrb_value invoke_block(mrb_state *mrb, mrb_value block, mrb_int argc, const mrb_value *argv)
{
    return mrb_funcall_argv(mrb, block, mrb_intern_cstr(mrb, "call"), argc, argv);
}

static int kind_from_symbol_value(mrb_state *mrb, mrb_value v, const char *what)
{
    if (mrb_type(v) != MRB_TT_SYMBOL) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "%s: kind: must be a symbol (:keyboard/:mouse/:consumer)", what);
    }
    const char *name = mrb_sym_name(mrb, mrb_symbol(v));
    if (strcmp(name, "keyboard") == 0) return PIPE_KEYBOARD;
    if (strcmp(name, "mouse") == 0) return PIPE_MOUSE;
    if (strcmp(name, "consumer") == 0) return PIPE_CONSUMER;
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "%s: unknown kind :%s (expected :keyboard/:mouse/:consumer)", what, name);
    return -1; // unreachable, mrb_raisef() is mrb_noreturn
}

static sink_def_t *find_sink(mrb_state *mrb, mrb_sym name)
{
    for (int i = 0; i < s_sink_count; i++) {
        if (s_sinks[i].name == name) {
            return &s_sinks[i];
        }
    }
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "unknown sink :%s (not declared with sink(...))", mrb_sym_name(mrb, name));
    return NULL; // unreachable
}

// ---- DSL methods: source / sink / pipeline / from / to / branch ------

static mrb_value dsl_source(mrb_state *mrb, mrb_value self)
{
    (void)self;
    mrb_sym name, type;
    mrb_value opts = mrb_nil_value();
    mrb_get_args(mrb, "nn|H", &name, &type, &opts);

    if (strcmp(mrb_sym_name(mrb, type), "usb_host") != 0) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
                   "source: unsupported type :%s (only :usb_host is implemented - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md)",
                   mrb_sym_name(mrb, type));
    }
    int kind = kind_from_symbol_value(mrb, dsl_opt(mrb, opts, "kind"), "source");

    if (s_source_count >= MRB_DSL_MAX_SINKS) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "source: too many sources declared");
    }
    s_sources[s_source_count].name = name;
    s_sources[s_source_count].kind = kind;
    s_source_count++;
    return mrb_nil_value();
}

static mrb_value dsl_sink(mrb_state *mrb, mrb_value self)
{
    (void)self;
    mrb_sym name, type;
    mrb_value opts = mrb_nil_value();
    mrb_get_args(mrb, "nn|H", &name, &type, &opts);

    if (s_sink_count >= MRB_DSL_MAX_SINKS) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "sink: too many sinks declared");
    }
    sink_def_t *sink = &s_sinks[s_sink_count];
    sink->name = name;
    sink->event_kind_hint = -1;

    const char *type_name = mrb_sym_name(mrb, type);
    if (strcmp(type_name, "typec") == 0) {
        sink->kind = SINK_TYPEC;
        mrb_value kind_v = dsl_opt(mrb, opts, "kind");
        if (!mrb_nil_p(kind_v)) {
            sink->event_kind_hint = kind_from_symbol_value(mrb, kind_v, "sink");
        }
    } else if (strcmp(type_name, "udp") == 0) {
        sink->kind = SINK_UDP;
        mrb_value host_v = dsl_opt(mrb, opts, "host");
        mrb_value port_v = dsl_opt(mrb, opts, "port");
        if (mrb_type(host_v) != MRB_TT_STRING || mrb_nil_p(port_v)) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "sink: :udp requires host: (String) and port: (Integer)");
        }
        // getaddrinfo() is deferred to mruby_filter_resolve_udp_sinks()
        // (called after WiFi is up) rather than done here: this DSL call
        // runs during mruby_filter_init(), which main_host.c calls
        // *before* wifi_manager_init() so mruby_filter_hostname() is
        // ready in time - but lwIP's TCP/IP thread isn't up yet at that
        // point, and calling getaddrinfo() here crashed with "assert
        // failed: tcpip_send_msg_wait_sem ... Invalid mbox" on real
        // hardware. See mds/usb_hid/2026-08-29_mruby_phase1_impl.md.
        mrb_int host_len = RSTRING_LEN(host_v);
        if ((size_t)host_len >= sizeof(sink->udp_host)) {
            host_len = sizeof(sink->udp_host) - 1;
        }
        memcpy(sink->udp_host, RSTRING_PTR(host_v), (size_t)host_len);
        sink->udp_host[host_len] = '\0';
        sink->udp_port = (int)mrb_fixnum(port_v);
        sink->udp_resolved = false;
    } else {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "sink: unsupported type :%s (only :typec/:udp are implemented)", type_name);
    }

    s_sink_count++;
    return mrb_nil_value();
}

static mrb_value dsl_from(mrb_state *mrb, mrb_value self)
{
    (void)self;
    mrb_sym name;
    mrb_get_args(mrb, "n", &name);

    for (int i = 0; i < s_source_count; i++) {
        if (s_sources[i].name == name) {
            s_building = true;
            s_building_kind = s_sources[i].kind;
            return mrb_nil_value();
        }
    }
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "from: unknown source :%s (not declared with source(...))", mrb_sym_name(mrb, name));
    return mrb_nil_value(); // unreachable
}

static mrb_value dsl_to(mrb_state *mrb, mrb_value self)
{
    (void)self;
    if (!s_building) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "to: called outside a pipeline block (call from(...) first)");
    }
    const mrb_value *names;
    mrb_int name_count;
    mrb_value blk = mrb_nil_value();
    mrb_get_args(mrb, "*&", &names, &name_count, &blk);

    pipeline_t *p = &s_pipelines[s_building_kind];
    if (!mrb_nil_p(blk)) {
        mrb_gc_register(mrb, blk); // must survive indefinitely - see mruby_dispatch_*() below
    }
    for (mrb_int i = 0; i < name_count; i++) {
        if (mrb_type(names[i]) != MRB_TT_SYMBOL) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "to: sink names must be symbols");
        }
        if (p->to_count >= MRB_DSL_MAX_STAGES) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "to: too many stages in this pipeline");
        }
        sink_def_t *sink = find_sink(mrb, mrb_symbol(names[i]));
        if (sink->event_kind_hint != -1 && sink->event_kind_hint != s_building_kind) {
            mrb_raisef(mrb, E_ARGUMENT_ERROR, "to: sink :%s was declared for a different kind", mrb_sym_name(mrb, sink->name));
        }
        p->to[p->to_count].sink  = sink;
        p->to[p->to_count].block = blk;
        p->to_count++;
    }
    return mrb_nil_value();
}

static mrb_value dsl_branch(mrb_state *mrb, mrb_value self)
{
    (void)self;
    if (!s_building) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "branch: called outside a pipeline block (call from(...) first)");
    }
    mrb_sym name;
    mrb_value blk;
    mrb_get_args(mrb, "n&!", &name, &blk);

    pipeline_t *p = &s_pipelines[s_building_kind];
    if (p->branch_count >= MRB_DSL_MAX_STAGES) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "branch: too many stages in this pipeline");
    }
    sink_def_t *sink = find_sink(mrb, name);
    if (sink->event_kind_hint != -1 && sink->event_kind_hint != s_building_kind) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "branch: sink :%s was declared for a different kind", mrb_sym_name(mrb, sink->name));
    }
    mrb_gc_register(mrb, blk);
    p->branch[p->branch_count].sink  = sink;
    p->branch[p->branch_count].block = blk;
    p->branch_count++;
    return mrb_nil_value();
}

static mrb_value dsl_pipeline(mrb_state *mrb, mrb_value self)
{
    (void)self;
    mrb_sym name;
    mrb_value blk;
    mrb_get_args(mrb, "n&!", &name, &blk);

    s_building = false;
    invoke_block(mrb, blk, 0, NULL);
    if (mrb->exc) {
        s_building = false;
        return mrb_nil_value(); // propagate - mruby_filter_init()'s check_error() will see this
    }
    if (!s_building) {
        mrb_raisef(mrb, E_ARGUMENT_ERROR, "pipeline :%s: block never called from(...)", mrb_sym_name(mrb, name));
    }
    s_building = false;
    return mrb_nil_value();
}

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

// ---- script loading -----------------------------------------------------

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
// mid-flight (mid-report, not just at boot) possible.
static bool check_error(void)
{
    if (s_mrb->exc) {
        mrb_print_error(s_mrb);
        s_mrb->exc = NULL;
        return true;
    }
    return false;
}

static void define_dsl_methods(mrb_state *mrb)
{
    struct RClass *k = mrb->kernel_module;
    mrb_define_method(mrb, k, "hostname", ruby_hostname, MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "source",   dsl_source,   MRB_ARGS_ARG(2, 1));
    mrb_define_method(mrb, k, "sink",     dsl_sink,     MRB_ARGS_ARG(2, 1));
    mrb_define_method(mrb, k, "pipeline", dsl_pipeline, MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_method(mrb, k, "from",     dsl_from,     MRB_ARGS_REQ(1));
    mrb_define_method(mrb, k, "to",       dsl_to,       MRB_ARGS_REST() | MRB_ARGS_BLOCK());
    mrb_define_method(mrb, k, "branch",   dsl_branch,   MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
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
    define_dsl_methods(s_mrb);

    bool loaded = false;
    reset_dsl_state();
    if (load_uploaded_script(s_mrb)) {
        if (check_error()) {
            ESP_LOGW(TAG, "Uploaded mrb_script failed to load, falling back to embedded default.rb");
        } else {
            loaded = true;
            ESP_LOGI(TAG, "Loaded uploaded script from mrb_script partition (no reflash)");
        }
    }
    if (!loaded) {
        reset_dsl_state(); // discard any partial pipeline state left by a failed upload
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

// Resolves every :udp sink's host/port (getaddrinfo()) - deferred out of
// dsl_sink()/mruby_filter_init() because lwIP's TCP/IP thread isn't up
// yet at that point (mruby_filter_init() runs before wifi_manager_init()
// so mruby_filter_hostname() is ready in time) - see dsl_sink()'s comment
// and mds/usb_hid/2026-08-29_mruby_phase1_impl.md. Call once, after
// wifi_manager_init() has returned successfully (main_host.c).
void mruby_filter_resolve_udp_sinks(void)
{
    if (!s_active) {
        return;
    }
    for (int i = 0; i < s_sink_count; i++) {
        sink_def_t *sink = &s_sinks[i];
        if (sink->kind != SINK_UDP || sink->udp_resolved) {
            continue;
        }
        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        char port_str[12];
        snprintf(port_str, sizeof(port_str), "%d", sink->udp_port);
        int err = getaddrinfo(sink->udp_host, port_str, &hints, &res);
        if (err != 0 || res == NULL) {
            ESP_LOGE(TAG, "sink :%s: failed to resolve host '%s' (err %d) - sends to it will be dropped",
                     mrb_sym_name(s_mrb, sink->name), sink->udp_host, err);
            continue;
        }
        memcpy(&sink->udp_addr, res->ai_addr, sizeof(sink->udp_addr));
        freeaddrinfo(res);
        sink->udp_resolved = true;
    }
}

// ---- per-event dispatch --------------------------------------------------

static mrb_value build_keyboard_event(mrb_state *mrb, uint8_t modifiers, const uint8_t keycodes[6])
{
    mrb_value h = mrb_hash_new_capa(mrb, 2);
    dsl_hset_int(mrb, h, "modifiers", modifiers);
    mrb_value kc = mrb_ary_new_capa(mrb, 6);
    for (int i = 0; i < 6; i++) {
        mrb_ary_push(mrb, kc, mrb_fixnum_value(keycodes[i]));
    }
    mrb_hash_set(mrb, h, dsl_hkey(mrb, "keycodes"), kc);
    return h;
}

static void read_keycodes(mrb_state *mrb, mrb_value h, uint8_t out[6], const uint8_t fallback[6])
{
    mrb_value kc = mrb_hash_get(mrb, h, dsl_hkey(mrb, "keycodes"));
    if (mrb_type(kc) != MRB_TT_ARRAY) {
        memcpy(out, fallback, 6);
        return;
    }
    mrb_int len = RARRAY_LEN(kc);
    for (int i = 0; i < 6; i++) {
        out[i] = (i < len) ? (uint8_t)mrb_fixnum(mrb_ary_ref(mrb, kc, i)) : 0;
    }
}

static void send_keyboard_to_sink(sink_def_t *sink, uint8_t modifiers, const uint8_t keycodes[6])
{
    if (sink->kind == SINK_TYPEC) {
        usb_device_typec_keyboard_report(modifiers, keycodes);
    } else if (sink->udp_resolved) {
        hid_forwarder_send_keyboard_to(&sink->udp_addr, modifiers, keycodes);
    }
}

void mruby_dispatch_keyboard(uint8_t modifiers, const uint8_t keycodes[6])
{
    int ai = mrb_gc_arena_save(s_mrb);
    pipeline_t *p = &s_pipelines[PIPE_KEYBOARD];

    for (int i = 0; i < p->to_count; i++) {
        to_stage_t *stage = &p->to[i];
        if (mrb_nil_p(stage->block)) {
            send_keyboard_to_sink(stage->sink, modifiers, keycodes);
            continue;
        }
        mrb_value ev = build_keyboard_event(s_mrb, modifiers, keycodes);
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (check_error() || mrb_nil_p(ret)) {
            continue; // script bug or explicit drop - skip this stage's sink this report
        }
        uint8_t o_modifiers = (uint8_t)dsl_hget_int(s_mrb, ev, "modifiers", modifiers);
        uint8_t o_keycodes[6];
        read_keycodes(s_mrb, ev, o_keycodes, keycodes);
        send_keyboard_to_sink(stage->sink, o_modifiers, o_keycodes);
    }
    for (int i = 0; i < p->branch_count; i++) {
        branch_stage_t *stage = &p->branch[i];
        mrb_value ev = build_keyboard_event(s_mrb, modifiers, keycodes); // always raw - see design doc
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (!check_error() && mrb_test(ret)) {
            send_keyboard_to_sink(stage->sink, modifiers, keycodes);
        }
    }

    mrb_gc_arena_restore(s_mrb, ai);
}

static mrb_value build_mouse_event(mrb_state *mrb, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    mrb_value h = mrb_hash_new_capa(mrb, 5);
    dsl_hset_int(mrb, h, "buttons", buttons);
    dsl_hset_int(mrb, h, "dx", dx);
    dsl_hset_int(mrb, h, "dy", dy);
    dsl_hset_int(mrb, h, "wheel", wheel);
    dsl_hset_int(mrb, h, "pan", pan);
    return h;
}

static void send_mouse_to_sink(sink_def_t *sink, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    if (sink->kind == SINK_TYPEC) {
        usb_device_typec_mouse_report(buttons, dx, dy, wheel, pan);
    } else if (sink->udp_resolved) {
        hid_forwarder_send_mouse_to(&sink->udp_addr, buttons, dx, dy, wheel, pan);
    }
}

void mruby_dispatch_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan,
                          uint8_t *synth_modifiers, uint8_t *synth_keycode)
{
    *synth_modifiers = 0;
    *synth_keycode   = 0; // HID_KEY_NO_PRESS == 0, see hid_usage_keyboard.h

    int ai = mrb_gc_arena_save(s_mrb);

    // Optional top-level hook, independent of sink routing - see
    // mruby_filter.h and mds/usb_hid/2026-08-29_mruby_phase1_impl.md for
    // why this isn't folded into a `to` block.
    if (mrb_respond_to(s_mrb, mrb_top_self(s_mrb), mrb_intern_cstr(s_mrb, "mouse_synth_keys"))) {
        mrb_value args[5] = {
            mrb_fixnum_value(buttons), mrb_fixnum_value(dx), mrb_fixnum_value(dy),
            mrb_fixnum_value(wheel), mrb_fixnum_value(pan),
        };
        mrb_value ret = mrb_funcall_argv(s_mrb, mrb_top_self(s_mrb),
                                          mrb_intern_cstr(s_mrb, "mouse_synth_keys"), 5, args);
        if (!check_error() && mrb_type(ret) == MRB_TT_ARRAY && RARRAY_LEN(ret) == 2) {
            *synth_modifiers = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 0));
            *synth_keycode   = (uint8_t)mrb_fixnum(mrb_ary_ref(s_mrb, ret, 1));
        }
    }

    pipeline_t *p = &s_pipelines[PIPE_MOUSE];
    for (int i = 0; i < p->to_count; i++) {
        to_stage_t *stage = &p->to[i];
        if (mrb_nil_p(stage->block)) {
            send_mouse_to_sink(stage->sink, buttons, dx, dy, wheel, pan);
            continue;
        }
        mrb_value ev = build_mouse_event(s_mrb, buttons, dx, dy, wheel, pan);
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (check_error() || mrb_nil_p(ret)) {
            continue;
        }
        uint8_t o_buttons = (uint8_t)dsl_hget_int(s_mrb, ev, "buttons", buttons);
        int16_t o_dx      = (int16_t)dsl_hget_int(s_mrb, ev, "dx", dx);
        int16_t o_dy      = (int16_t)dsl_hget_int(s_mrb, ev, "dy", dy);
        int8_t o_wheel    = (int8_t)dsl_hget_int(s_mrb, ev, "wheel", wheel);
        int8_t o_pan      = (int8_t)dsl_hget_int(s_mrb, ev, "pan", pan);
        send_mouse_to_sink(stage->sink, o_buttons, o_dx, o_dy, o_wheel, o_pan);
    }
    for (int i = 0; i < p->branch_count; i++) {
        branch_stage_t *stage = &p->branch[i];
        mrb_value ev = build_mouse_event(s_mrb, buttons, dx, dy, wheel, pan); // always raw
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (!check_error() && mrb_test(ret)) {
            send_mouse_to_sink(stage->sink, buttons, dx, dy, wheel, pan);
        }
    }

    mrb_gc_arena_restore(s_mrb, ai);
}

static mrb_value build_consumer_event(mrb_state *mrb, uint16_t usage_id)
{
    mrb_value h = mrb_hash_new_capa(mrb, 1);
    dsl_hset_int(mrb, h, "usage_id", usage_id);
    return h;
}

static void send_consumer_to_sink(sink_def_t *sink, uint16_t usage_id)
{
    if (sink->kind == SINK_TYPEC) {
        usb_device_typec_consumer_report(usage_id);
    } else if (sink->udp_resolved) {
        hid_forwarder_send_consumer_to(&sink->udp_addr, usage_id);
    }
}

void mruby_dispatch_consumer(uint16_t usage_id)
{
    int ai = mrb_gc_arena_save(s_mrb);
    pipeline_t *p = &s_pipelines[PIPE_CONSUMER];

    for (int i = 0; i < p->to_count; i++) {
        to_stage_t *stage = &p->to[i];
        if (mrb_nil_p(stage->block)) {
            send_consumer_to_sink(stage->sink, usage_id);
            continue;
        }
        mrb_value ev = build_consumer_event(s_mrb, usage_id);
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (check_error() || mrb_nil_p(ret)) {
            continue;
        }
        uint16_t o_usage_id = (uint16_t)dsl_hget_int(s_mrb, ev, "usage_id", usage_id);
        send_consumer_to_sink(stage->sink, o_usage_id);
    }
    for (int i = 0; i < p->branch_count; i++) {
        branch_stage_t *stage = &p->branch[i];
        mrb_value ev = build_consumer_event(s_mrb, usage_id);
        mrb_value ret = invoke_block(s_mrb, stage->block, 1, &ev);
        if (!check_error() && mrb_test(ret)) {
            send_consumer_to_sink(stage->sink, usage_id);
        }
    }

    mrb_gc_arena_restore(s_mrb, ai);
}
