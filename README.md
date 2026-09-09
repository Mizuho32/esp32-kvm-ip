# ESP32 KVM over IP

Wireless KVM switch (keyboard + mouse) based on ESP32-S3. Captures input from a **Host PC** (Windows) and forwards it over WiFi/UDP to an ESP32-S3, which emulates a standard USB HID device (keyboard + mouse) connected to a **Target PC**.

The Target PC sees a regular USB keyboard and mouse, no drivers or software required.

## Features

- Full keyboard support: all keys, modifiers (L/R Ctrl, Shift, Alt, GUI), 6-key rollover, function keys, numpad
- 5-button mouse with 16-bit relative movement, vertical and horizontal scroll
- Raw mouse data (Raw Input) – no Windows acceleration, 1:1 sensor mapping
- KVM mode toggle via **Scroll Lock** – input goes either to Host or Target
- **Clipboard paste** – Shift+Insert types clipboard text as keystrokes on Target (supports ASCII + Polish diacritics, Escape to cancel)
- Fixed polling rate of 125 Hz (configurable 60–1000 Hz) with mouse movement accumulation
- **Invisible Mouse Jiggler** (`--jiggle`) – prevents the Target PC from sleeping by sending 0-pixel mouse movements
- Binary UDP protocol – 16-byte packets, low latency
- WiFi Modem Sleep disabled – eliminates ~200 ms lag on first packet

## System Diagram

```
┌─────────────────────┐         Wi-Fi / UDP           ┌──────────────────────┐
│     HOST PC         │  ────────────────────────►    │       ESP32-S3       │
│                     │    port 4210                  │                      │
│  server.py          │    16B packets @ 125 Hz       │  network_task        │
│  ├─ WH_KEYBOARD_LL  │                               │  (UDP recv + parse)  │
│  ├─ WH_MOUSE_LL     │                               │         │            │
│  ├─ Raw Input       │                               │    xQueue (32)       │
│  └─ Sender Thread   │                               │         │            │
│     (accumulate+UDP)│                               │  hid_task            │
│                     │                               │  (tud_hid_report)    │
└─────────────────────┘                               │         │ USB        │
                                                      └─────────┼────────────┘
                                                                │
                                                      ┌─────────▼────────────┐
                                                      │     TARGET PC        │
                                                      │  sees: keyboard      │
                                                      │  + mouse USB HID     │
                                                      └──────────────────────┘
```

## Hardware Requirements

- **ESP32-S3** – any board with native USB OTG (e.g. ESP32-S3-DevKitC-1)
- USB cable to connect ESP32-S3 to the **Target PC** (via USB OTG port, not UART)
- Host PC and ESP32-S3 on the same WiFi network

## Software Requirements

| Component | Requirements |
|---|---|
| **ESP32 Firmware** | ESP-IDF v5.x, components: `esp_tinyusb`, `tinyusb` (fetched automatically) |
| **Server (Host PC)** | Python 3.10+. Windows: no external dependencies (WinAPI hooks + Raw Input via ctypes). Linux: `pip install -r server/requirements-linux.txt` (keyboard only — see below) |

## Installation

### ESP32-S3 Firmware

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) (v5.x)

2. Clone the repository:
   ```
   git clone <repo-url>
   cd esp32-kvm-ip
   ```

3. (Host role only) Set the Device-role board's forwarding target:
   ```
   cp main/wifi_credentials.h.example main/wifi_credentials.h
   ```
   Edit `main/wifi_credentials.h` and set `KVM_TARGET_HOST`. This file is
   gitignored (kept out of the repo) and, unlike a Kconfig value, editing
   it only recompiles the couple of files that include it instead of the
   whole project. WiFi SSID/password are *not* set here - see step 5.

4. Build and flash:
   ```
   idf.py build flash
   ```
   (a full `flash`, not just the app image - this writes the partition
   table too, which the board needs on first flash)

5. Upload WiFi credentials over serial (not a compile-time constant -
   this writes to a flash partition instead, so real credentials never
   end up in the repo/build tree):
   ```
   export ESP_IDF=/opt/esp-idf && source "$ESP_IDF/export.sh"
   ../bin/upload_wifi_credentials.py --port /dev/ttyUSB0 --ssid "My WiFi" --hostname esp32-kvm-ip
   ```
   Prompts for the password interactively (hidden, not echoed - never
   pass it as a command-line argument). `--hostname` is optional - sent
   to the DHCP server (option 12), so the device shows up under that name
   in your router's DHCP lease list instead of just an IP address -
   useful for finding the ESP32's IP on routers like OpenWRT without a
   static lease. Reset/power-cycle the board afterwards to connect.
   The ESP32 will connect to WiFi and start listening on UDP port 4210.

   Note: on boards with a single native-USB port (e.g. XIAO ESP32S3), that
   port is claimed by the HID device once the app starts, so `idf.py monitor`
   won't show any output there. Console logs are routed to UART0 instead —
   connect an external USB-UART adapter to the board's UART0 TX/RX pins and
   open that serial port (e.g. `idf.py -p <uart-port> monitor`) to view logs.

### Server (Host PC)

**Windows**: no external dependencies, uses only the Python standard library.

```
cd server
python server.py --host <ESP32_IP>
```

**Linux**: keyboard-only for now (mouse capture isn't ported yet — the
Windows-only WinAPI hooks don't apply). Uses `evdev` + `EVIOCGRAB`, so it works under X11 or Wayland (any
desktop environment, including Wayland/KDE Plasma) since evdev sits below
the display server.

```
cd server
pip install -r requirements-linux.txt
python server.py --host <ESP32_IP>
```

Requires read/write access to `/dev/input/event*`:

```
sudo usermod -aG input $USER   # then log out and back in
```

Clipboard paste (Shift+Insert) needs `wl-paste` (wl-clipboard, Wayland) or
`xclip`/`xsel` (X11) installed; without any of them, paste silently does
nothing.

## Usage

The onboard user LED (GPIO21 on XIAO ESP32S3) turns on once WiFi is
connected and an IP address has been obtained, as a visual "ready" signal
that doesn't require a serial monitor.

1. Connect the ESP32-S3 via USB to the **Target PC** (USB OTG port)
2. Run the server on the **Host PC**:
   ```
   python server.py --host 192.168.1.21
   ```
3. Press **Scroll Lock** to toggle KVM mode:
   - **KVM OFF** (default) – keyboard and mouse work normally on Host PC
   - **KVM ON** – input is blocked on Host PC and forwarded to Target PC
     (Linux: keyboard only — the mouse is never captured, so it keeps
     working normally on the Host PC even while KVM is on)

### Clipboard Paste

While KVM is active, press **Shift+Insert** to type the Host clipboard contents on the Target PC as individual keystrokes. This is useful for pasting passwords, commands, URLs, or any text into a machine that has no network/shared clipboard.

- Supported characters: ASCII (letters, digits, punctuation, whitespace) and Polish diacritics (ą, ć, ę, ł, ń, ó, ś, ź, ż via AltGr)
- Polish characters require the **Polish Programmer** keyboard layout on both Host and Target
- Unsupported characters (e.g. emoji, CJK) are silently skipped
- Press **Escape** to cancel paste in progress
- Typing speed: ~62 chars/s at 125 Hz (1 press + 1 release per character).

### Server Options

| Parameter | Default | Description |
|---|---|---|
| `--host` | *(required)* | ESP32-S3 IP address |
| `--port` | 4210 | UDP port |
| `--rate` | 125 | Polling rate in Hz (60–1000) |
| `--jiggle` | *(optional)* | Enable invisible mouse jiggler |

## Architecture

### UDP Protocol

Fixed packet size: **16 bytes**, little-endian. Event type (mouse/keyboard) specified in the header. Each packet carries the full state (not deltas) of buttons and keys, a lost packet won't cause a "stuck key". A monotonic sequence counter allows ESP32 to discard duplicate and stale packets.

### Server (Windows)

- **WH_KEYBOARD_LL** – captures keystrokes, maps VK → HID Usage ID, blocks propagation to Host when KVM is active
- **WH_MOUSE_LL** – blocks mouse on Host PC when KVM is active
- **Raw Input (WM_INPUT)** – reads raw mouse deltas without Windows acceleration
- **Sender Thread** – fixed polling rate, accumulates dx/dy from Raw Input, packs and sends UDP. Prevents flooding the ESP32 (gaming mice generate 1000+ events/s, while the ESP queue holds only 32 slots)

### ESP32-S3 Firmware

Two FreeRTOS tasks pinned to separate cores:
- **network_task (Core 0)** – receives UDP packets, validates (magic + sequence), parses and pushes to xQueue
- **hid_task (Core 1)** – dequeues events and sends HID reports over USB (TinyUSB)

Single USB HID device, two collections distinguished by Report ID:
- **Report ID 1 – Mouse:** 5 buttons, 16-bit X/Y (relative), 8-bit wheel + pan
- **Report ID 2 – Keyboard:** 8-bit modifiers, 6-key rollover, 5 LEDs (output)

USB polling interval: 1 ms. WiFi Modem Sleep disabled.

### Key Design Decisions

| Decision | Rationale |
|---|---|
| UDP without ACK | Lowest latency. Full state in every packet compensates for packet loss |
| 16-bit X/Y mouse | 8-bit (±127) is insufficient when accumulating movement between WiFi packets |
| Raw Input instead of LL hook for mouse | Bypasses Windows acceleration, 1:1 sensor movement |
| Fixed polling rate with accumulation | Throughput control, ESP32 is not flooded with thousands of events/s |
| Report ID instead of 2 interfaces | Simpler: 1 HID interface, 1 endpoint, smaller descriptor |
| WiFi Modem Sleep = OFF | Default power saving adds ~200 ms lag on first packet |
| Task pinning to cores | Network + WiFi stack on Core 0, HID on Core 1, no contention |

## mruby DSL: Event (`ev`) Reference

*(Host role only, `KVM_ROLE=HOST`.)* The Host role can load an mruby script (`main/mruby_scripts/default.rb`, replaceable via the WebUI or `bin/upload_mruby_script.py`) that declares `source`/`sink`/`pipeline` blocks routing HID events between USB Host input, USB-C device output, BLE HID output and UDP. See `mds/usb_hid/2026-08-28_mruby_filter_route.md` for the full DSL design - this section only documents the event object itself.

A `pipeline`'s `to`/`branch` block is called with one argument, `ev` - a plain mruby `Hash` with **Symbol** keys (not a dot-accessor object), rebuilt fresh from the current raw HID report on every call. Its shape depends on the pipeline's `kind` (`:keyboard`, `:mouse` or `:consumer`, aka "cc"):

### `:keyboard`

| key | type | notes |
|---|---|---|
| `:modifiers` | Integer, 0–255 | Standard USB HID keyboard modifier bitmask (bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGui, bit4 RCtrl, bit5 RShift, bit6 RAlt, bit7 RGui - TinyUSB's `KEYBOARD_MODIFIER_*` enum) |
| `:keycodes` | Array of 6 Integers | 6-key rollover slots, each a USB HID Keyboard/Keypad Usage ID; `0` = empty slot. Mutate in place (`ev[:keycodes].map! { ... }`) or assign a new Array - fewer than 6 elements pads with 0, extras beyond 6 are ignored |

### `:mouse`

| key | type | notes |
|---|---|---|
| `:buttons` | Integer, 0–255 | Bitmask, bit0=button1/left, bit1=button2/right, bit2=button3/middle, bit3=button4/back, bit4=button5/forward - test with e.g. `ev[:buttons] & (1 << 3)` |
| `:dx`, `:dy` | Integer, -32768..32767 | Relative movement since the last report (not accumulated/absolute) |
| `:wheel` | Integer, -128..127 | Vertical scroll delta |
| `:pan` | Integer, -128..127 | Horizontal scroll delta |

### `:consumer` (cc)

| key | type | notes |
|---|---|---|
| `:usage_id` | Integer, 0–65535 | Consumer Page (0x0C) HID Usage ID - e.g. play/pause, volume up/down, mute (see TinyUSB's `HID_USAGE_CONSUMER_*` constants for the numeric values). `0` = release/no key |

### Behavior notes

- **`to` block**: return a Hash to send - any key you don't touch (or that gets removed) falls back to the raw value that was about to be sent, not to any default of your own. Returning `nil` drops the send for that stage/sink entirely (e.g. to conditionally suppress an event). Mutating `ev` in place and returning it works, since it's the same Hash object passed in (see `main/mruby_scripts/examples/key_remap.rb`).
- **`branch` block**: always receives the *raw* (un-mutated) event, regardless of what any `to` block in the same pipeline did to its own copy - return truthy to forward the raw event to that branch's sink, falsy to skip it (see `main/mruby_scripts/examples/wheel_to_udp_only.rb`).
- All values are plain mruby Integers regardless of the underlying C type's width - a `to` block that returns an out-of-range value (e.g. `:modifiers` > 255) gets silently truncated/wrapped when narrowed back to the wire type, not rejected.
- `mouse_synth_keys(buttons, dx, dy, wheel, pan) -> [modifiers, keycode]`, if defined at the script's top level, is a separate hook (not a pipeline block) called once per local mouse sample to optionally synthesize a keyboard key press alongside it (e.g. mouse back/forward buttons → Alt+Left/Right) - see `main/mruby_scripts/examples/wheel_to_udp_only.rb`.

## Project Structure

```
esp32-kvm-ip/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── main.c                 # Initialization: NVS, WiFi, TinyUSB, task creation
│   ├── Kconfig.projbuild      # USB descriptor strings config (menuconfig)
│   ├── tusb_config.h          # TinyUSB configuration
│   ├── usb_descriptors.c/h    # USB HID descriptors + callbacks
│   ├── protocol.h             # UDP packet structures + event types
│   ├── wifi_manager.c/h       # WiFi STA initialization (SSID/password read from the wifi_cred partition, see bin/upload_wifi_credentials.py)
│   ├── wifi_credentials.h.example  # Copy to wifi_credentials.h (gitignored) and edit - KVM_TARGET_HOST only, not WiFi credentials
│   ├── status_led.c/h         # Onboard LED (on once WiFi is up)
│   ├── network_task.c/h       # UDP receive → xQueue
│   └── hid_task.c/h           # xQueue → USB HID reports
└── server/
    ├── server.py              # Entry point: picks the input backend for the OS
    ├── state.py               # Shared thread-safe input state
    ├── udp_sender.py          # State → UDP packets at a fixed rate
    ├── protocol.py            # UDP packet packing
    ├── winapi_hooks.py        # Windows: LL hooks + Raw Input (keyboard + mouse)
    ├── hid_keymap.py          # Windows: VK_* → HID Usage ID mapping
    ├── evdev_hooks.py         # Linux: evdev + EVIOCGRAB (keyboard only)
    ├── evdev_keymap.py        # Linux: KEY_* → HID Usage ID mapping
    ├── requirements-linux.txt # Linux: pip dependencies (evdev)
    └── clipboard_typer.py     # Clipboard paste: text → HID keystroke sequences
```

## License

MIT
