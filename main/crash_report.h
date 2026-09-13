// Crash safety net - see mds/usb_hid/2026-09-13_crash_reporting.md.
//
// ESP-IDF's own coredump-to-flash (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
// sdkconfig.defaults) writes a full ELF core dump (registers, backtrace,
// per-task stacks) to the `coredump` partition (partitions.csv) whenever
// the board panics or a watchdog fires - but that raw dump only survives
// until *this* module reads and erases it on the very next boot. This
// file is what turns that one-shot flash blob into something durable and
// visible: a short human-readable summary saved in NVS (so it survives
// any number of further *clean* reboots - opening the WebUI a week later
// still shows the last crash, not just "the previous boot"), surfaced in
// the WebUI (mruby_webui.c) and optionally pushed to ntfy.sh.
//
// Also home to the `heap_trace_start`/`heap_trace_dump` mruby DSL
// commands (mruby_filter.c) - an on-demand (not always-on) tool for the
// next time a leak like mds/usb_hid/2026-09-13_ble_idle_crash.md's needs
// hunting down to an exact malloc() call site.
#ifndef _CRASH_REPORT_H_
#define _CRASH_REPORT_H_

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once, early in app_main() - right after nvs_flash_init(), before
// anything else touches NVS or the coredump partition. Checks
// esp_reset_reason(): if this boot followed a panic/watchdog/brownout
// reset *and* a valid core dump is present, saves a short summary into
// NVS (namespace "crash", survives future clean reboots) and erases the
// coredump partition (so the next real crash gets a clean slot - the NVS
// copy is the durable record from here on, not the partition itself).
//
// If the new summary is byte-identical to the one already saved (same
// panic reason/task/PC - a boot loop crashing on the same bug over and
// over), this bumps a repeat count instead of treating it as a new
// crash - see crash_report_last_text()'s "[recurred Nx]" suffix - and
// does *not* re-arm crash_report_notify_after_wifi() below, so a boot
// loop pushes exactly one ntfy notification, not one per crash.
// crash_report_clear() resets that: the next occurrence, even of this
// same unfixed bug, is "new" again and notifies once more - that's the
// "mark this crash resolved" mechanism, reusing the WebUI's existing
// dismiss button rather than needing a separate one.
void crash_report_init(void);

// Call once, right after wifi_manager_start() succeeds - registers a
// one-shot IP_EVENT_STA_GOT_IP handler that fires the ntfy.sh push (if
// configured - see mruby_filter.h's ble_dynamic-style opt-in, this one's
// via the script's `crash_notify_url` DSL call) exactly once, only if
// crash_report_init() found a *new* (not a repeat of the last-notified
// one) crash this boot. No-op on an ordinary boot or a repeat.
void crash_report_notify_after_wifi(void);

// Sets the ntfy.sh (or any plain-HTTP-POST-body) URL to push a crash
// summary to - e.g. "https://ntfy.sh/my-topic". `url`/`len` mirror
// mruby_filter.c's own ruby_hostname()/ruby_ntp_sync() string-arg
// pattern (not necessarily NUL-terminated at exactly `len`) since,
// like WiFi credentials/hostname, this board has no compile-time-fixed
// notification endpoint - the script's DSL is the only source. len==0
// disables the push (WebUI/NVS reporting still works regardless).
void crash_report_set_notify_url(const char *url, size_t len);

// Renders the last saved crash summary (if any), with a trailing
// "[recurred Nx since last cleared]" if the same crash has happened more
// than once - into `out` (NUL-terminated, truncated to fit). Empty
// string if there's no saved crash. Reads NVS fresh each call so it's
// always current - see mruby_webui.c's status page.
void crash_report_last_text(char *out, size_t out_size);

// Clears the saved crash summary *and* its repeat count (mruby_webui.c's
// dismiss button, POST /api/crash_clear) - see crash_report_init()'s
// comment on why clearing, not just viewing, is what re-arms
// notification for this same crash if it recurs. Doesn't touch the
// coredump partition (already erased by crash_report_init() regardless).
void crash_report_clear(void);

// `heap_trace_start` / `heap_trace_dump` mruby DSL commands
// (mruby_filter.c) - see crash_report.c's own doc comment on why this
// exists. Lazily allocates the trace buffer (PSRAM) on first call, then
// reuses it - repeated heap_trace_start() calls just restart tracing
// into the same buffer. crash_report_heap_trace_dump() stops tracing and
// prints every still-allocated record (size + call-site backtrace) via
// ESP_LOG - read it from the same place as any other boot/debug log.
esp_err_t crash_report_heap_trace_start(void);
void crash_report_heap_trace_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* _CRASH_REPORT_H_ */
