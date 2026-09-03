# NeoPixel jacket — project documentation

A wearable with two independently controllable 20-pixel NeoPixel strips (left and right), driven by a Bluefruit Feather nRF52832 over Bluetooth Low Energy.

## Hardware

| Component | Spec |
|---|---|
| Microcontroller | Adafruit Feather nRF52832 (Bluefruit) |
| LED strips | 2x WS2812/NeoPixel, 20 pixels each |
| Data pins | A0 (left strip), A1 (right strip) |
| Power | USB power bank, 2x 5V/1A outputs |
| USB-serial bridge | CP2104 (needs Silicon Labs CP210x VCP driver on Windows) |

### Wiring

- Left strip data: Feather A0 → 470 Ω resistor → strip DIN
- Right strip data: Feather A1 → 470 Ω resistor → strip DIN
- Power bank port 1 → Feather USB
- Power bank port 2 → both strips' 5V/GND
- **Common ground is required**: strip GND, power bank GND, and Feather GND must all be tied together. (This was the fix for the "everything connects, nothing lights" issue during bring-up.)
- 1000 µF capacitor across each strip's 5V/GND, close to the first pixel — smooths inrush current at power-on and when brightness jumps.
- A diode (e.g. 1N4001) in series with the strips' 5V feed is worth adding since the strips run on a real 5V rail while the Feather's data output is 3.3V; this drops the strip rail slightly so the 3.3V logic level clears the WS2812 threshold more reliably.

### Power budget

- Two strips (40 pixels) at full white ≈ 2.4 A — well above what the power bank's 1A ports supply. Brightness is capped in firmware (`MAX_BRIGHTNESS`, default 100 of 255) to stay within budget.
- Idle draw with all pixels dark is still roughly 1 mA per pixel (~40 mA total) just to keep the WS2812 controller chips powered.
- Power banks often auto-shutoff below ~50–100 mA draw. The firmware can hold one pixel dimly lit in "off" mode (`KEEPALIVE_LEVEL`) to prevent this; set to 0 if your bank doesn't have this behavior.

## Firmware

Arduino sketch (`Adafruit nRF52` board package, board: **Adafruit Bluefruit nRF52832 Feather**) using `Adafruit_NeoPixel` and the Bluefruit BLE stack.

**Modes**: Solid, Rainbow, Chase, Breathe, Off — selectable per strip or both together.

**BLE service**: Nordic UART Service (NUS), the same service used by Adafruit's Bluefruit Connect app.

- Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (app → jacket, write): `6e400002-...`
- TX (jacket → app, notify): `6e400003-...`

**Text command protocol** (newline-terminated ASCII, sent over the RX characteristic):

| Command | Effect |
|---|---|
| `l R G B` | Set left strip to color, switch to solid mode |
| `r R G B` | Set right strip to color, switch to solid mode |
| `a R G B` | Set both strips to color, switch to solid mode |
| `l` / `r` / `a` (no args) | Set target to left/right/both without changing color |
| `b N` | Brightness, 1–100 |
| `m N` | Mode: 0 solid, 1 rainbow, 2 chase, 3 breathe, 4 off |

The device also responds to Bluefruit Connect's **Controller** module (Color Picker + Control Pad buttons) via the binary packet protocol. It does **not** support Bluefruit Connect's dedicated NeoPixel module — that module expects a different, single-strip-only protocol and will report "not detected."

Safety limits baked into firmware:
- `MAX_BRIGHTNESS` caps the brightness ceiling regardless of what any app requests.
- `limitColor()` caps the combined R+G+B of any single pixel (`MAX_CHANNEL_SUM`), so no color request can exceed the current budget — Rainbow mode is the one exception, since it can still push all 40 pixels bright at once.

## Control options

### 1. Bluefruit Connect (phone app)
Quickest way to test. Use the **Controller** module (not the NeoPixel module — see above). Color Picker sets color on the current target; Control Pad buttons switch target/mode/brightness.

### 2. Custom web controller (this project's primary test tool)
A single self-contained HTML file (`jacket_controller.html`) using the Web Bluetooth API. Runs in Chrome on Android only (no iOS Safari support). No install, no server required — open the file directly in Chrome.

Built for user testing specifically:
- Fixed color swatches instead of a color picker, to remove color-selection as a source of variance between trials.
- Large touch targets for target (left/right/both), mode, and brightness.
- On-screen, timestamped log of every command sent and every acknowledgement received back from the jacket, useful for verifying trial timing during a session.

### 3. PC control (for scripted test sessions)
Not yet built, but the same text protocol works over either:
- USB serial (`pyserial`) if the participant is tethered, or
- BLE (`bleak`, cross-platform) for untethered testing.

A PC script is the better choice if trials need fixed, repeatable timing rather than an operator clicking through conditions live.

## Known issues / lessons from bring-up

- **CP2104 driver**: on Windows, the board won't show up in Arduino IDE's port list until the Silicon Labs CP210x VCP driver is installed — this applies to the Feather nRF52832 as well as boards like the Metro Mini, since both use the same USB-serial bridge chip.
- **Common ground**: the most common "looks connected, nothing lights" failure. Always verify continuity between the power bank ground, strip ground, and Feather ground.
- **Baud rate mismatch**: garbage output in the Serial Monitor that appears exactly in time with button presses means data is arriving correctly but being decoded at the wrong baud rate — set the monitor to 115200.
- **DIN vs DOUT**: strips only accept data on the arrow-in end; reversed wiring gives no error, just a dark strip.

## Possible next steps

- Build the PC (BLE or USB) control script for scripted test sequences.
- Add write acknowledgements to the firmware protocol so a PC test rig can confirm each command landed rather than assuming.
- Consider a standalone physical controller (a second nRF52 board acting as BLE Central) if the phone/PC dependency becomes limiting for field testing.
