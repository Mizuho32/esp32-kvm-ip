#ifndef MRUBY_WEBUI_H
#define MRUBY_WEBUI_H

// Host role only (KVM_ROLE=HOST). Phase 2 of
// mds/usb_hid/2026-08-28_mruby_filter_route.md: a small HTTP server
// (esp_http_server) serving a single page to view/edit the mruby script
// from a browser - no serial/parttool.py round-trip needed (Phase 1,
// bin/upload_mruby_script.py, still works and remains available). See
// mds/usb_hid/2026-08-30_mruby_phase2_webui.md for the design and
// implementation notes.
//
// Must be called after WiFi is up - same ordering as
// mruby_filter_resolve_udp_sinks()/mruby_filter_start_net_source()
// (main_host.c calls all three together, right after
// wifi_manager_init() succeeds). No separate build-time switch from
// CONFIG_MRUBY_FILTER_ROUTE_ENABLE: if mruby itself isn't compiled in,
// the WebUI has nothing useful to edit, so this is a no-op in that case.
void mruby_webui_start(void);

#endif
