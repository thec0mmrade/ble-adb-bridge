# BLE-to-ADB Bridge

Use modern Bluetooth keyboards and mice with a vintage Macintosh SE.

This firmware turns a [Heltec WiFi LoRa 32 V3](https://heltec.org/project/wifi-lora-32-v3/) (ESP32-S3) into a bridge between Bluetooth Low Energy HID devices and the Apple Desktop Bus. The ESP32 connects to BLE keyboards and mice as a Central, then emulates two ADB devices (keyboard at address 2, mouse at address 3) on the Mac's single-wire bus.

## Hardware

### Parts

| Part | Purpose |
|------|---------|
| Heltec WiFi LoRa 32 V3 | Microcontroller (ESP32-S3, BLE 5.0, OLED) |
| BSS138 N-channel MOSFET | Bidirectional 3.3V/5V level shifter |
| 2x 1k&ohm; resistors | Pull-ups for each side of the level shifter |
| Mini-DIN 4 male plug | ADB connector (or splice an existing cable) |

### Wiring

The ADB bus is open-collector at 5V. The ESP32-S3 GPIOs are 3.3V and **not** 5V tolerant. A BSS138 MOSFET provides bidirectional level shifting (same circuit used in I2C level shifter breakouts from SparkFun/Adafruit).

```
Mac SE ADB Port (Mini-DIN 4)         Heltec V3
Pin 3 (+5V) ──── 1k&ohm; ────┐
                              │
                         BSS138 drain
                          gate ── 3.3V
                         BSS138 source
                              │
               3.3V ── 1k&ohm; ────┼──── GPIO48
                              │
Pin 1 (DATA) ────────────────┘
Pin 4 (GND)  ── shared GND ──────── GND
Pin 2 (PSW)  ── not connected
```

**Mini-DIN 4 pinout** (looking at the plug from the front):

```
  4   3
   . .
   . .
  1   2
```

| Pin | Signal | Connection |
|-----|--------|------------|
| 1 | DATA | Level shifter to GPIO48 |
| 2 | PSW (power switch) | Not connected |
| 3 | +5V | 1k pull-up on 5V side of level shifter |
| 4 | GND | Shared ground with Heltec |

### Quick Option: Series Resistor Only

If you don't have a BSS138 MOSFET, a single 100-150&ohm; resistor works for development. This relies on the ESP32-S3's internal clamping diodes and the Mac's own 1k&ohm; pull-up to limit current.

```
ADB Pin 1 (DATA) ──── 100&ohm; ──── GPIO48
ADB Pin 4 (GND)  ─────────────── GND
```

**How it works:** When the bus is idle at 5V, the ESP32's clamping diode clamps the pin to ~3.6V and the Mac's 1k&ohm; pull-up limits current to ~1.3mA (safe). When the ESP32 drives low, the Mac sees 5V &times; 100 / (1000 + 100) = 0.45V (valid logic low; ADB requires < 0.8V).

| Series R | Bus low voltage | Verdict |
|----------|----------------|---------|
| 100&ohm; | 0.45V | Good |
| 150&ohm; | 0.65V | Good |
| 220&ohm; | 0.9V | Borderline |
| 330&ohm;+ | >1.2V | Won't work |

**Caveats:** This relies on undocumented internal clamping diodes. It works in practice (widely used for 5V I2C on ESP32) but is not officially specified by Espressif. For a permanent installation, use the BSS138 level shifter.

### Using a Breakout Board

If you have a SparkFun or Adafruit BSS138 level shifter breakout board, the wiring simplifies to:

| Breakout Pin | Connection |
|--------------|------------|
| LV | Heltec 3.3V |
| HV | ADB Pin 3 (+5V) |
| GND | Shared ground |
| LV1 | GPIO48 |
| HV1 | ADB Pin 1 (DATA) |

The breakout includes the pull-up resistors and MOSFET, so no loose components are needed.

### ADB Connector

The ADB port uses a Mini-DIN 4 connector (same physical connector as S-Video). Options for obtaining one:

- Cut and strip a cable from a dead ADB keyboard or mouse
- Buy a Mini-DIN 4 male plug (widely available, search "S-Video plug" or "Mini-DIN 4 male solder")

Only 3 of the 4 pins are used: DATA (pin 1), +5V (pin 3), and GND (pin 4).

### Power

During development, USB-C powers the Heltec and provides serial debug. For standalone use, the ADB +5V rail (pin 3, up to 500mA) can power the Heltec through its 5V input with a Schottky diode to prevent USB backfeed.

## Building

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) (CLI) or PlatformIO IDE

### Compile and Flash

```bash
pio run -t upload
```

PlatformIO will download the ESP32-S3 toolchain, NimBLE-Arduino, and the SSD1306 OLED driver automatically.

### Serial Monitor

```bash
pio device monitor
```

Baud rate is 115200. The firmware logs boot diagnostics, BLE connection events, ADB timing self-test results, and (with verbose mode enabled) every keypress and mouse movement.

## Usage

1. Flash the firmware and power on the Heltec
2. The OLED shows "BLE-ADB Bridge / Initializing..."
3. Put your BLE keyboard and/or mouse into pairing mode
4. The bridge auto-discovers HID devices and connects (the OLED updates to show device names and status)
5. Connect the ADB cable to your Mac SE
6. The Mac polls the bus; the bridge responds as keyboard (address 2) and mouse (address 3)

The bridge automatically reconnects if a BLE device disconnects, and continuously scans for missing devices.

## Architecture

```
[BLE Keyboard] ──BLE──┐
                       ├── Core 0: NimBLE HID Host ── FreeRTOS Queues ──┐
[BLE Mouse]    ──BLE──┘                                                 │
                                                                        v
                                            Core 1: ADB Protocol Loop
                                              +-- Keyboard (addr 2)
                                              +-- Mouse (addr 3)
                                                       |
                                                 GPIO48 + BSS138
                                                       |
                                                 Mac SE ADB Port
```

The ESP32-S3 dual cores are fully isolated:

- **Core 0** runs BLE scanning/connection (NimBLE), HID report parsing, and OLED display updates
- **Core 1** runs the ADB bus loop with bit-banged timing (interrupts disabled during bit I/O)

FreeRTOS queues (`kbd_event_queue`, `mouse_event_queue`) bridge the cores lock-free.

### Module Map

```
ble-adb-bridge/
+-- platformio.ini              PlatformIO project config
+-- include/
|   +-- config.h                Pins, timing constants, compile-time flags
|   +-- adb_platform.h         GPIO register access + microsecond timing HAL
|   +-- adb_protocol.h         ADB bus-level bit I/O + command state machine
|   +-- adb_keyboard.h         Keyboard device emulation (address 2)
|   +-- adb_mouse.h            Mouse device emulation (address 3)
|   +-- ble_hid_host.h         BLE Central: scan, connect, parse HID reports
|   +-- keycode_map.h          USB HID keycode to ADB keycode translation
|   +-- event_queue.h          FreeRTOS queue wrappers + event types
|   +-- oled_display.h         Status display on Heltec onboard OLED
+-- src/
    +-- main.cpp                Dual-core task setup, initialization sequence
    +-- adb_platform.cpp        Direct GPIO register I/O for GPIO48
    +-- adb_protocol.cpp        Bus loop, attention detection, command dispatch
    +-- adb_keyboard.cpp        Key event buffer, Talk/Listen/Flush handlers
    +-- adb_mouse.cpp           Delta accumulation, 7-bit clamping, button inversion
    +-- ble_hid_host.cpp        NimBLE scan/connect, device type detection, report parsing
    +-- keycode_map.cpp         256-entry USB-to-ADB lookup table
    +-- event_queue.cpp         Queue creation and send/receive helpers
    +-- oled_display.cpp        Non-blocking OLED rendering at 4 Hz
```

### Data Flow

**Keyboard:** BLE HID report (8 bytes) -> diff modifier byte + 6-key array against previous state -> translate each changed key via `keycode_map::usb_to_adb()` -> push `KbdEvent` to queue -> ADB keyboard dequeues, packs up to 2 keycodes into 16-bit Talk Register 0 response

**Mouse:** BLE HID report (3-7 bytes) -> extract button + X/Y deltas -> push `MouseEvent` to queue -> ADB mouse accumulates deltas between polls, clamps to signed 7-bit (-64..+63), inverts button (ADB: 1=released) -> Talk Register 0 response

## ADB Protocol

Apple Desktop Bus is a single-wire, open-collector, half-duplex serial bus. The Mac is the host and polls each device in turn.

### Bus Timing

| Signal | Duration |
|--------|----------|
| Attention (host pulls low) | 800 &mu;s nominal (560-1040) |
| Sync (high after attention) | 65 &mu;s nominal |
| Bit cell total | 100 &mu;s |
| Bit '1': low / high | 35 &mu;s / 65 &mu;s |
| Bit '0': low / high | 65 &mu;s / 35 &mu;s |
| Stop-to-start (Tlt) | 200 &mu;s (max 260) |
| Global reset | >2800 &mu;s low |
| SRQ | 300 &mu;s low during stop bit |

### Command Format

The host sends an 8-bit command after attention + sync:

```
[4-bit address] [2-bit command] [2-bit register]
```

| Command | Code | Direction |
|---------|------|-----------|
| Reset | 00 | Host -> Device |
| Flush | 01 | Host -> Device |
| Listen | 10 | Host -> Device (followed by 16-bit data) |
| Talk | 11 | Device -> Host (device sends 16-bit data) |

### Keyboard Response (Talk Register 0)

```
[R1][7-bit ADB keycode 1] [R2][7-bit ADB keycode 2]
```

R = 1 for key release, 0 for key press. Second slot is 0xFF if only one key event is pending.

### Mouse Response (Talk Register 0)

```
[button][7-bit Y delta] [1][7-bit X delta]
```

Button: 1 = released, 0 = pressed. Deltas are signed 7-bit two's complement. Bit 8 of the second byte is always 1.

### Service Request (SRQ)

When the host polls a different device and this bridge has pending data, it extends the stop bit's low phase to 300 &mu;s to signal the host to come back sooner.

## Configuration

All configuration is in `include/config.h`. Key settings:

| Constant | Default | Description |
|----------|---------|-------------|
| `ADB_DATA_PIN` | 48 | GPIO pin for ADB data line |
| `ADB_DEBUG_VERBOSE` | 0 | Log every ADB command and BLE event to serial |
| `ADB_SELF_TEST` | 0 | Run bit-timing self-test at boot |
| `ADB_BUS_MONITOR` | 0 | Passive bus monitor mode (no device emulation) |
| `ADB_TASK_PRIORITY` | 5 | FreeRTOS priority for ADB bus loop |
| `BLE_TASK_PRIORITY` | 3 | FreeRTOS priority for BLE task |

Override compile-time flags via `build_flags` in `platformio.ini`:

```ini
build_flags =
    -DADB_DEBUG_VERBOSE=0
    -DADB_SELF_TEST=0
```

### Bus Monitor Mode

Set `ADB_BUS_MONITOR=1` to make the bridge passively listen to an existing ADB bus and decode all traffic to serial. Useful for studying the Mac's polling pattern with a real keyboard connected.

## BLE Device Compatibility

The bridge auto-detects device type by checking for Boot Protocol characteristics (Boot Keyboard Input Report `0x2A22`, Boot Mouse Input Report `0x2A33`) and falls back to parsing the HID Report Map descriptor.

**Tested devices:**

| Device | Type | Status |
|--------|------|--------|
| NuPhy Air75 V2 | Keyboard | All keys verified |
| Lofree Touch | Mouse/Trackpad | Movement + click verified |

Both HID Boot Protocol (3-byte mouse, 8-byte keyboard) and Report Protocol (variable-length with 16-bit deltas) formats are supported.

## Resource Usage

| Resource | Usage |
|----------|-------|
| Flash | 532 KB (25% of 2 MB) |
| RAM | 30 KB (9% of 320 KB) |
| Free heap at runtime | ~262 KB (with both devices connected) |

## Debugging

### Serial Output

At 115200 baud, the firmware logs:

- Boot banner with CPU frequency, free heap, ADB pin
- ADB timing self-test results (bit-by-bit timing measurements)
- BLE scan results, connection attempts, device type detection
- Every key press/release with USB and ADB keycodes (when verbose)
- Mouse movement deltas (when verbose)
- Periodic status line: heap, keyboard/mouse connection state

### OLED Display

The onboard 128x64 OLED shows:

```
KBD: [OK] NuPhy Air75 V2
MOU: [OK] Touch@LOFREE
ADB: ACTIVE  Rate:91/s
Polls:48210 Events:347
```

### Self-Test

At boot (when `ADB_SELF_TEST=1`), the firmware measures actual bit timing:

```
[ADB] Testing '1' bit timing (expect ~35us low, ~65us high):
  [0] low=36us high=67us total=103us
[ADB] Testing '0' bit timing (expect ~65us low, ~35us high):
  [0] low=66us high=37us total=103us
```

The measured values should be within ~5 &mu;s of the spec. ADB has generous tolerances (~30%).

## References

- [Apple ADB Protocol](https://www.lopaciuk.eu/2021/03/26/apple-adb-protocol.html) - device-side timing reference
- [QEMU adb-keys.h](https://github.com/qemu/qemu/blob/master/include/hw/input/adb-keys.h) - authoritative ADB wire scan codes (use instead of Apple's Cosmo_USB2ADB.c which uses Mac virtual keycodes)
- [QuokkADB](https://github.com/rabbitholecomputing/QuokkADB-firmware) - RP2040 USB-to-ADB bridge, keycode table reference
- [TMK adb.c](https://github.com/tmk/tmk_keyboard/blob/master/tmk_core/protocol/adb.c) - AVR host-side ADB implementation
- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) - BLE stack
- [Heltec V3 Pin Diagram](https://resource.heltec.cn/download/WiFi_LoRa_32_V3/HTIT-WB32LA(F)_V3_Schematic_Diagram.pdf) - GPIO map and schematic

## License

MIT
