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
| **Server (Host PC)** | Python 3.10+, Windows (WinAPI hooks + Raw Input via ctypes, no external dependencies) |

## Installation

### ESP32-S3 Firmware

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) (v5.x)

2. Clone the repository:
   ```
   git clone <repo-url>
   cd esp32-kvm-ip
   ```

3. Configure WiFi SSID and password:
   ```
   cp main/wifi_credentials.h.example main/wifi_credentials.h
   ```
   Edit `main/wifi_credentials.h` and set `WIFI_SSID` / `WIFI_PASSWORD`.
   This file is gitignored (kept out of the repo) and, unlike a Kconfig
   value, editing it only recompiles the couple of files that include it
   instead of the whole project.

4. Build and flash:
   ```
   idf.py build flash
   ```
   The ESP32 will connect to WiFi and start listening on UDP port 4210.

   Note: on boards with a single native-USB port (e.g. XIAO ESP32S3), that
   port is claimed by the HID device once the app starts, so `idf.py monitor`
   won't show any output there. Console logs are routed to UART0 instead —
   connect an external USB-UART adapter to the board's UART0 TX/RX pins and
   open that serial port (e.g. `idf.py -p <uart-port> monitor`) to view logs.

### Server (Host PC)

The server has no external dependencies, it uses only the Python standard library.

```
cd server
python server.py --host <ESP32_IP>
```

## Usage

1. Connect the ESP32-S3 via USB to the **Target PC** (USB OTG port)
2. Run the server on the **Host PC**:
   ```
   python server.py --host 192.168.1.21
   ```
3. Press **Scroll Lock** to toggle KVM mode:
   - **KVM OFF** (default) – keyboard and mouse work normally on Host PC
   - **KVM ON** – input is blocked on Host PC and forwarded to Target PC

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
│   ├── wifi_manager.c/h       # WiFi STA initialization
│   ├── wifi_credentials.h.example  # Copy to wifi_credentials.h (gitignored) and edit
│   ├── network_task.c/h       # UDP receive → xQueue
│   └── hid_task.c/h           # xQueue → USB HID reports
└── server/
    ├── server.py              # Server: WinAPI hooks, Raw Input, UDP sender
    ├── clipboard_typer.py     # Clipboard paste: text → HID keystroke sequences
    ├── hid_keymap.py          # VK_* → HID Usage ID mapping (150+ keys)
    └── protocol.py            # UDP packet packing
```

## License

MIT
