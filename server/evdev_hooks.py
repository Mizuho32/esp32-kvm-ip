"""
Linux input capture backend (evdev + EVIOCGRAB).

Keyboard-only for now (see mds/usb_hid/2026-08-20_server.md for rationale).
Mouse state in InputState is left untouched, so the Host PC's mouse
keeps working normally regardless of KVM mode.

Devices are opened (but not grabbed) at start() and read continuously,
mirroring the always-listening WinAPI low-level hook. EVIOCGRAB
(device.grab()/ungrab()) is toggled on Scroll Lock to start/stop
blocking propagation to the rest of the system (X11/Wayland/console) -
our own process keeps receiving events from a grabbed device, so Scroll
Lock stays observable even while KVM is active, same as the Windows
backend always seeing the hook callback regardless of block state.
"""

import selectors

from evdev import InputDevice, ecodes, list_devices

from evdev_keymap import EV_MODIFIER_MAP, EV_TO_HID, EV_TO_CONSUMER
from state import InputState

VK_INSERT_EQUIVALENT_SHIFT_MASK = 0x22  # bit1 (LShift) | bit5 (RShift)


def _discover_keyboard_devices() -> list[InputDevice]:
    """Every EV_KEY-capable device that isn't a mouse/trackpad/touchscreen."""
    devices = []
    for path in list_devices():
        try:
            dev = InputDevice(path)
        except OSError:
            continue
        caps = dev.capabilities()
        if ecodes.EV_REL in caps or ecodes.EV_ABS in caps:
            dev.close()
            continue
        if ecodes.EV_KEY not in caps:
            dev.close()
            continue
        devices.append(dev)
    return devices


class InputHookManager:
    """Same interface as winapi_hooks.InputHookManager."""

    def __init__(self, state: InputState):
        self.state = state
        self.devices: list[InputDevice] = []
        self.selector = selectors.DefaultSelector()
        self._grabbed = False
        self._running = False

    def start(self):
        self.devices = _discover_keyboard_devices()
        if not self.devices:
            raise RuntimeError(
                "No accessible keyboard-like input devices found under "
                "/dev/input. Add your user to the 'input' group "
                "(sudo usermod -aG input $USER, then log out/in) and retry."
            )
        for dev in self.devices:
            self.selector.register(dev, selectors.EVENT_READ)
        names = ", ".join(f"{d.name!r} ({d.path})" for d in self.devices)
        print(f"[INIT] Watching {len(self.devices)} device(s): {names}")

    def stop(self):
        self._running = False
        for dev in self.devices:
            try:
                self.selector.unregister(dev)
            except KeyError:
                pass
            try:
                if self._grabbed:
                    dev.ungrab()
            except OSError:
                pass
            dev.close()

    def process_messages(self):
        self._running = True
        while self._running:
            for key, _mask in self.selector.select():
                dev: InputDevice = key.fileobj
                try:
                    for event in dev.read():
                        if event.type == ecodes.EV_KEY:
                            self._process_key(event)
                except OSError:
                    # Device unplugged.
                    self.selector.unregister(dev)
                    self.devices.remove(dev)

    def _set_grabbed(self, grab: bool):
        self._grabbed = grab
        for dev in self.devices:
            try:
                dev.grab() if grab else dev.ungrab()
            except OSError as ex:
                print(f"[WARN] {'grab' if grab else 'ungrab'} failed on {dev.path}: {ex}")

    def _process_key(self, event):
        code = event.code
        if event.value == 2:  # autorepeat
            return
        is_down = event.value == 1

        if code == ecodes.KEY_SCROLLLOCK:
            if is_down:
                with self.state.lock:
                    self.state.kvm_active = not self.state.kvm_active
                    active = self.state.kvm_active
                    if not active:
                        self.state.reset_state()
                self._set_grabbed(active)
                print(f"[KVM] {'ON' if active else 'OFF'}")
            return

        with self.state.lock:
            if not self.state.kvm_active:
                return

            if code == ecodes.KEY_INSERT and is_down and (self.state.modifiers & VK_INSERT_EQUIVALENT_SHIFT_MASK):
                self.state.start_paste()
                return

            if self.state.pasting:
                if code == ecodes.KEY_ESC and is_down:
                    self.state.cancel_paste()
                return

            if code in EV_MODIFIER_MAP:
                bit = EV_MODIFIER_MAP[code]
                if is_down:
                    self.state.modifiers |= (1 << bit)
                else:
                    self.state.modifiers &= ~(1 << bit)
                self.state.kbd_dirty = True
            elif code in EV_TO_CONSUMER:
                usage = EV_TO_CONSUMER[code]
                if is_down:
                    self.state.consumer_usage = usage
                elif self.state.consumer_usage == usage:
                    self.state.consumer_usage = 0
                self.state.consumer_dirty = True
            else:
                hid_code = EV_TO_HID.get(code)
                if hid_code is not None:
                    if is_down:
                        self.state.pressed_keys.add(hid_code)
                    else:
                        self.state.pressed_keys.discard(hid_code)
                    self.state.kbd_dirty = True
