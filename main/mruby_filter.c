#include "mruby_filter.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mruby.h"
#include "mruby/array.h"
#include "mruby/compile.h"
#include "mruby/hash.h"
#include "mruby/string.h"

#include "hid_forwarder.h"
#include "protocol.h"
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
// objects, mouse_synth_keys as a separate hook, and how `:udp` sources -
// which unlike `:usb_host` carry any kind, decided per from()'s kind:
// option - are dispatched through a parallel set of pipelines).
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

typedef enum { SRC_USB_HOST, SRC_UDP } source_type_t;

typedef struct {
    mrb_sym name;
    source_type_t type;
    int kind;        // PIPE_* - fixed at declare time for SRC_USB_HOST; unused for SRC_UDP (a :udp source carries any kind, decided per from()'s kind: opt - see dsl_from())
    int listen_port;  // SRC_UDP only
} source_def_t;

static sink_def_t s_sinks[MRB_DSL_MAX_SINKS];
static int s_sink_count;
static source_def_t s_sources[MRB_DSL_MAX_SINKS];
static int s_source_count;

// Local (:usb_host-sourced) and network (:udp-sourced) pipelines are kept
// in separate kind-indexed arrays - see mruby_dispatch_keyboard/mouse/consumer()
// (local, called from hid_forwarder.c) vs. mruby_dispatch_net_keyboard/mouse/consumer()
// (network, called from net_source_task() below). A :udp source has no
// fixed kind of its own (unlike :usb_host - a physical mouse only ever
// produces mouse events), so which of the 3 net pipelines a `from :net_in,
// kind: :mouse` targets is decided by from()'s kind: option instead of by
// the source itself.
static pipeline_t s_pipelines[PIPE_KIND_COUNT];
static pipeline_t s_net_pipelines[PIPE_KIND_COUNT];

// Set by from() while inside a pipeline {...} block; to()/branch() append
// to s_pipelines[s_building_kind] (or s_net_pipelines[], if s_building_net)
// while this is true. Pipelines never nest, so no stack is needed - see
// dsl_pipeline().
static bool s_building;
static bool s_building_net;
static int s_building_kind;

// At most one :udp source (one listen port) is supported - see dsl_source().
static bool s_net_source_declared;
static int s_net_source_port;
static TaskHandle_t s_net_source_task;

// `usb_host_backends(*syms)` - see mruby_filter.h. -1 = not explicitly
// configured by the script; resolved to a default in mruby_filter_init()
// once the whole script has loaded (so s_net_source_declared is final).
#define MAX_HOST_BACKENDS 3
static mruby_host_backend_t s_host_backends[MAX_HOST_BACKENDS];
static int s_host_backend_count = -1;
static bool s_host_backends_explicit;

static mrb_state *s_mrb;
// mrb_state isn't thread-safe - once net_source_task() exists, a script
// with both local (:usb_host-sourced) pipelines and a :udp source can
// have dispatch_task/bridge_task (local input) and net_source_task
// (network input) call into the same VM from two different FreeRTOS
// tasks concurrently. Every mruby_dispatch_*()/mruby_dispatch_net_*()
// call takes this for its whole body - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md
// (this was found from a real "checksum mismatch"/heap-corruption-looking
// regression once a script exercised both paths at once).
static SemaphoreHandle_t s_mrb_mutex;
static bool s_active;
static char s_hostname[64];
static bool s_hostname_set;

static void reset_dsl_state(void)
{
    s_sink_count = 0;
    s_source_count = 0;
    memset(s_pipelines, 0, sizeof(s_pipelines));     // only zeroes to_count/branch_count meaningfully - see reset_dsl_state()'s call sites
    memset(s_net_pipelines, 0, sizeof(s_net_pipelines));
    s_building = false;
    s_building_net = false;
    s_building_kind = 0;
    s_net_source_declared = false;
    s_net_source_port = -1;
    s_host_backend_count = -1;
    s_host_backends_explicit = false;
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

static mruby_host_backend_t host_backend_from_symbol(mrb_state *mrb, mrb_value v)
{
    if (mrb_type(v) != MRB_TT_SYMBOL) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "usb_host_backends: arguments must be symbols");
    }
    const char *name = mrb_sym_name(mrb, mrb_symbol(v));
    if (strcmp(name, "rp2040_bridge") == 0) return MRUBY_HOST_BACKEND_RP2040_BRIDGE;
    if (strcmp(name, "max3421") == 0) return MRUBY_HOST_BACKEND_MAX3421;
    if (strcmp(name, "native_otg") == 0) return MRUBY_HOST_BACKEND_NATIVE_OTG;
    mrb_raisef(mrb, E_ARGUMENT_ERROR,
               "usb_host_backends: unknown backend :%s (expected :rp2040_bridge/:max3421/:native_otg)", name);
    return MRUBY_HOST_BACKEND_RP2040_BRIDGE; // unreachable
}

// `usb_host_backends :rp2040_bridge, :max3421, :native_otg` - order = try
// order, main_host.c stops at the first that actually starts. Omitting a
// backend disables it entirely; omitting :native_otg frees it for type-c
// device output instead of USB Host input. See mruby_filter.h.
static mrb_value dsl_usb_host_backends(mrb_state *mrb, mrb_value self)
{
    (void)self;
    const mrb_value *argv;
    mrb_int argc;
    mrb_get_args(mrb, "*", &argv, &argc);
    if (argc > MAX_HOST_BACKENDS) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "usb_host_backends: too many backends listed");
    }
    for (mrb_int i = 0; i < argc; i++) {
        s_host_backends[i] = host_backend_from_symbol(mrb, argv[i]);
    }
    s_host_backend_count = (int)argc;
    s_host_backends_explicit = true;
    return mrb_nil_value();
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

    if (s_source_count >= MRB_DSL_MAX_SINKS) {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "source: too many sources declared");
    }
    source_def_t *src = &s_sources[s_source_count];
    src->name = name;

    const char *type_name = mrb_sym_name(mrb, type);
    if (strcmp(type_name, "usb_host") == 0) {
        src->type = SRC_USB_HOST;
        src->kind = kind_from_symbol_value(mrb, dsl_opt(mrb, opts, "kind"), "source");
    } else if (strcmp(type_name, "udp") == 0) {
        mrb_value listen_v = dsl_opt(mrb, opts, "listen");
        if (mrb_nil_p(listen_v)) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "source: :udp requires listen: (Integer port)");
        }
        // Only one listen port is supported (one board, one socket) - see
        // net_source_task()/mruby_filter_start_net_source().
        if (s_net_source_declared) {
            mrb_raise(mrb, E_ARGUMENT_ERROR, "source: only one :udp source is supported (multiple `source ..., :udp` calls)");
        }
        src->type = SRC_UDP;
        src->listen_port = (int)mrb_fixnum(listen_v);
        s_net_source_declared = true;
        s_net_source_port = src->listen_port;
    } else {
        mrb_raisef(mrb, E_ARGUMENT_ERROR,
                   "source: unsupported type :%s (only :usb_host/:udp are implemented - see mds/usb_hid/2026-08-29_mruby_phase1_impl.md)",
                   type_name);
    }

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
    mrb_value opts = mrb_nil_value();
    mrb_get_args(mrb, "n|H", &name, &opts);

    for (int i = 0; i < s_source_count; i++) {
        if (s_sources[i].name != name) {
            continue;
        }
        source_def_t *src = &s_sources[i];
        s_building = true;
        if (src->type == SRC_USB_HOST) {
            s_building_net = false;
            s_building_kind = src->kind;
        } else { // SRC_UDP - no fixed kind of its own, from() must say which
            mrb_value kind_v = dsl_opt(mrb, opts, "kind");
            if (mrb_nil_p(kind_v)) {
                mrb_raisef(mrb, E_ARGUMENT_ERROR,
                           "from: :udp source :%s requires kind: (it carries any kind - a separate pipeline per kind picks which)",
                           mrb_sym_name(mrb, name));
            }
            s_building_net = true;
            s_building_kind = kind_from_symbol_value(mrb, kind_v, "from");
        }
        return mrb_nil_value();
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

    pipeline_t *p = s_building_net ? &s_net_pipelines[s_building_kind] : &s_pipelines[s_building_kind];
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

    pipeline_t *p = s_building_net ? &s_net_pipelines[s_building_kind] : &s_pipelines[s_building_kind];
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
    s_building_net = false;
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

// Finds the mrb_script partition and reads/validates just its 4-byte
// length header (no content read, no allocation) - returns NULL if it's
// missing/erased/corrupt, else the partition (with *out_len set) so the
// caller can read the content directly at the right offset. Shared by
// read_script_partition_raw() (below), mruby_filter_read_script(), and
// mruby_filter_script_len().
static const esp_partition_t *find_mrb_script_partition(uint32_t *out_len)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, MRB_SCRIPT_PARTITION_SUBTYPE, MRB_SCRIPT_PARTITION_LABEL);
    if (part == NULL) {
        return NULL;
    }
    uint32_t len;
    if (esp_partition_read(part, 0, &len, sizeof(len)) != ESP_OK) {
        return NULL;
    }
    if (len == 0 || len == 0xFFFFFFFFu || len > part->size - sizeof(len)) {
        return NULL; // never uploaded (erased flash reads as 0xFF) or corrupt
    }
    *out_len = len;
    return part;
}

// Used by load_uploaded_script() (below), which needs its own buffer
// (mrb_load_nstring() doesn't read straight from flash). mruby_filter_read_script()
// deliberately does NOT go through this - see its comment.
static bool read_script_partition_raw(char **out_buf, uint32_t *out_len)
{
    uint32_t len;
    const esp_partition_t *part = find_mrb_script_partition(&len);
    if (part == NULL) {
        return false;
    }

    char *buf = malloc(len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "read_script_partition_raw: malloc(%" PRIu32 ") failed", len);
        return false;
    }
    esp_err_t err = esp_partition_read(part, sizeof(len), buf, len);
    if (err != ESP_OK) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    *out_len = len;
    return true;
}

// Returns true if a valid uploaded script was found and mrb_load_nstring()'d
// (caller must still check_error() - a script can be present but fail to
// parse). False means the partition is missing/erased/corrupt - caller
// should fall back to the embedded default.rb, not treat this as fatal.
static bool load_uploaded_script(mrb_state *mrb)
{
    char *buf;
    uint32_t len;
    if (!read_script_partition_raw(&buf, &len)) {
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
    mrb_define_method(mrb, k, "from",     dsl_from,     MRB_ARGS_ARG(1, 1));
    mrb_define_method(mrb, k, "to",       dsl_to,       MRB_ARGS_REST() | MRB_ARGS_BLOCK());
    mrb_define_method(mrb, k, "branch",   dsl_branch,   MRB_ARGS_REQ(1) | MRB_ARGS_BLOCK());
    mrb_define_method(mrb, k, "usb_host_backends", dsl_usb_host_backends, MRB_ARGS_REST());
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
    s_mrb_mutex = xSemaphoreCreateMutex();
    if (s_mrb_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex() failed, falling back to C filter_rules.h/route_rules.h");
        mrb_close(s_mrb);
        s_mrb = NULL;
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

    if (!s_host_backends_explicit) {
        // Script didn't call usb_host_backends() - a :udp source means
        // this board relies on network-received input, so it has no use
        // for native-OTG-as-host and needs type-c actually started for
        // its :udp-sourced pipelines' :typec sinks to work. Otherwise,
        // default to the original hardcoded probe order. See
        // mruby_filter.h.
        s_host_backends[0] = MRUBY_HOST_BACKEND_RP2040_BRIDGE;
        s_host_backends[1] = MRUBY_HOST_BACKEND_MAX3421;
        if (s_net_source_declared) {
            s_host_backend_count = 2;
        } else {
            s_host_backends[2] = MRUBY_HOST_BACKEND_NATIVE_OTG;
            s_host_backend_count = 3;
        }
    }

    s_active = true;
    ESP_LOGI(TAG, "mruby VM active (hostname %s)", s_hostname_set ? s_hostname : "not set by script");
#endif
}

bool mruby_filter_active(void)
{
    return s_active;
}

int mruby_filter_host_backend_count(void)
{
    return s_active ? s_host_backend_count : 0;
}

mruby_host_backend_t mruby_filter_host_backend_at(int index)
{
    return s_host_backends[index];
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

static void dispatch_keyboard_via(pipeline_t *p, uint8_t modifiers, const uint8_t keycodes[6])
{
    int ai = mrb_gc_arena_save(s_mrb);

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

// Local (:usb_host-sourced) keyboard report - called from hid_forwarder.c.
void mruby_dispatch_keyboard(uint8_t modifiers, const uint8_t keycodes[6])
{
    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);
    dispatch_keyboard_via(&s_pipelines[PIPE_KEYBOARD], modifiers, keycodes);
    xSemaphoreGive(s_mrb_mutex);
}

// Network (:udp-sourced) keyboard report - called from net_source_task() below.
static void mruby_dispatch_net_keyboard(uint8_t modifiers, const uint8_t keycodes[6])
{
    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);
    dispatch_keyboard_via(&s_net_pipelines[PIPE_KEYBOARD], modifiers, keycodes);
    xSemaphoreGive(s_mrb_mutex);
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

static void dispatch_mouse_via(pipeline_t *p, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    int ai = mrb_gc_arena_save(s_mrb);

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

// Local (:usb_host-sourced) mouse sample - called from hid_forwarder.c.
// Also runs the script's optional mouse_synth_keys hook - see
// mruby_filter.h. Network-sourced mouse samples
// (mruby_dispatch_net_mouse() below) deliberately skip this: a
// network-received sample already went through whatever synth-key logic
// the *sending* board's own pipeline applied, so re-deriving synth keys
// here from a remote board's raw buttons would be redundant/wrong.
void mruby_dispatch_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan,
                          uint8_t *synth_modifiers, uint8_t *synth_keycode)
{
    *synth_modifiers = 0;
    *synth_keycode   = 0; // HID_KEY_NO_PRESS == 0, see hid_usage_keyboard.h

    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);

    int ai = mrb_gc_arena_save(s_mrb);
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
    mrb_gc_arena_restore(s_mrb, ai);

    dispatch_mouse_via(&s_pipelines[PIPE_MOUSE], buttons, dx, dy, wheel, pan);

    xSemaphoreGive(s_mrb_mutex);
}

// Network (:udp-sourced) mouse sample - called from net_source_task() below.
static void mruby_dispatch_net_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);
    dispatch_mouse_via(&s_net_pipelines[PIPE_MOUSE], buttons, dx, dy, wheel, pan);
    xSemaphoreGive(s_mrb_mutex);
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

static void dispatch_consumer_via(pipeline_t *p, uint16_t usage_id)
{
    int ai = mrb_gc_arena_save(s_mrb);

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

// Local (:usb_host-sourced) consumer report - called from hid_forwarder.c.
void mruby_dispatch_consumer(uint16_t usage_id)
{
    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);
    dispatch_consumer_via(&s_pipelines[PIPE_CONSUMER], usage_id);
    xSemaphoreGive(s_mrb_mutex);
}

// Network (:udp-sourced) consumer report - called from net_source_task() below.
static void mruby_dispatch_net_consumer(uint16_t usage_id)
{
    xSemaphoreTake(s_mrb_mutex, portMAX_DELAY);
    dispatch_consumer_via(&s_net_pipelines[PIPE_CONSUMER], usage_id);
    xSemaphoreGive(s_mrb_mutex);
}

// ---- network source (:udp) --------------------------------------------

static void net_source_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "net source: socket() failed: errno %d", errno);
        s_net_source_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons((uint16_t)s_net_source_port),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "net source: bind(port %d) failed: errno %d", s_net_source_port, errno);
        close(sock);
        s_net_source_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "net source: listening for HID events on UDP port %d", s_net_source_port);

    udp_packet_t pkt;
    while (1) {
        int len = recvfrom(sock, &pkt, sizeof(pkt), 0, NULL, NULL);
        if (len < 0) {
            ESP_LOGW(TAG, "net source: recvfrom() failed: errno %d", errno);
            continue;
        }
        if (len != PACKET_SIZE || pkt.magic != PACKET_MAGIC) {
            ESP_LOGW(TAG, "net source: dropping malformed packet (len=%d, expected %d; magic=0x%04x, expected 0x%04x)",
                     len, PACKET_SIZE, pkt.magic, PACKET_MAGIC);
            continue; // malformed or non-protocol traffic on this port - ignore
        }
        switch (pkt.type) {
        case EVENT_TYPE_KEYBOARD:
            mruby_dispatch_net_keyboard(pkt.keyboard.modifiers, pkt.keyboard.keycodes);
            break;
        case EVENT_TYPE_MOUSE:
            mruby_dispatch_net_mouse(pkt.mouse.buttons, pkt.mouse.dx, pkt.mouse.dy, pkt.mouse.wheel, pkt.mouse.pan);
            break;
        case EVENT_TYPE_CONSUMER:
            mruby_dispatch_net_consumer(pkt.consumer.usage_id);
            break;
        default:
            break;
        }
    }
}

void mruby_filter_start_net_source(void)
{
    if (!s_active || !s_net_source_declared) {
        return;
    }
    if (xTaskCreate(net_source_task, "mruby_net_src", 4096, NULL, 5, &s_net_source_task) != pdPASS) {
        ESP_LOGE(TAG, "net source: failed to start task");
    }
}

// ---- WebUI support (Phase 2, mruby_webui.c) ----------------------------

// Deliberately reads straight from the partition into the caller's buf
// (via find_mrb_script_partition() + esp_partition_read()) rather than
// going through read_script_partition_raw()'s malloc'd temp buffer +
// memcpy: mruby_webui.c's GET /api/script calls this on every page
// load, and briefly holding two copies of a script that can be up to
// 64K (the fixed-size buffer callers pre-allocate, per mruby_filter.h,
// plus this function's own malloc'd copy) was a real cause of
// intermittent "out of memory" 500s from the WebUI - the ESP32-S3 here
// has no PSRAM enabled (CONFIG_SPIRAM is off), so everything competes
// for the same ~300-400K of internal SRAM alongside the mruby VM heap,
// WiFi/lwIP buffers, and every task's stack. See
// mds/usb_hid/2026-08-30_mruby_phase2_webui.md.
size_t mruby_filter_read_script(char *buf, size_t buf_size)
{
    if (buf_size == 0) {
        return 0;
    }
    uint32_t raw_len;
    const esp_partition_t *part = find_mrb_script_partition(&raw_len);
    if (part != NULL) {
        size_t n = ((size_t)raw_len < buf_size - 1) ? (size_t)raw_len : buf_size - 1;
        if (esp_partition_read(part, sizeof(raw_len), buf, n) == ESP_OK) {
            buf[n] = '\0';
            return n;
        }
    }
    size_t dlen = (size_t)(mruby_default_script_end - mruby_default_script_start);
    size_t n = (dlen < buf_size - 1) ? dlen : buf_size - 1;
    memcpy(buf, mruby_default_script_start, n);
    buf[n] = '\0';
    return n;
}

// Lets mruby_webui.c's GET /api/script allocate a buffer sized to the
// script's actual length instead of always allocating the worst-case
// MAX_SCRIPT_SIZE - see mruby_filter_read_script()'s comment on why
// over-allocating here matters on this board.
size_t mruby_filter_script_len(void)
{
    uint32_t raw_len;
    if (find_mrb_script_partition(&raw_len) != NULL) {
        return (size_t)raw_len;
    }
    return (size_t)(mruby_default_script_end - mruby_default_script_start);
}

esp_err_t mruby_filter_write_script(const char *new_script, size_t new_len)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, MRB_SCRIPT_PARTITION_SUBTYPE, MRB_SCRIPT_PARTITION_LABEL);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (new_len > part->size - sizeof(uint32_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Whole-partition erase (like parttool.py write_partition, what
    // bin/upload_mruby_script.py uses) - mrb_script is a single 64K
    // erase-sector-aligned partition, so this is one erase op, not
    // per-write-call granularity.
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t len_hdr = (uint32_t)new_len;
    err = esp_partition_write(part, 0, &len_hdr, sizeof(len_hdr));
    if (err != ESP_OK) {
        return err;
    }
    if (new_len > 0) {
        err = esp_partition_write(part, sizeof(len_hdr), new_script, new_len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
