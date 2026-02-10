# BLE-ADB Bridge Developer Guide

Firmware for the ESP32-S3 (Heltec WiFi LoRa 32 V3) that bridges Bluetooth Low Energy HID keyboards and mice to the Apple Desktop Bus, allowing modern wireless input devices to work with vintage Macintosh computers.

## Table of Contents

- [Getting Started](#getting-started)
- [Architecture](#architecture)
- [Source File Reference](#source-file-reference)
- [ADB Protocol Implementation](#adb-protocol-implementation)
- [BLE HID Host](#ble-hid-host)
- [Inter-Core Communication](#inter-core-communication)
- [Keycode Translation](#keycode-translation)
- [OLED Display](#oled-display)
- [Diagnostics and Debugging](#diagnostics-and-debugging)
- [Hardware Setup](#hardware-setup)
- [Configuration Reference](#configuration-reference)
- [Pitfalls and Hard-Won Lessons](#pitfalls-and-hard-won-lessons)

---

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE plugin)
- Heltec WiFi LoRa 32 V3 board (ESP32-S3)
- USB cable (data-capable, not charge-only)

### Build and Flash

```bash
pio run                  # compile only
pio run -t upload        # compile and flash
pio device monitor       # serial monitor (115200 baud)
```

### Dependencies

Managed by PlatformIO (declared in `platformio.ini`):

| Library | Version | Purpose |
|---------|---------|---------|
| NimBLE-Arduino | ^2.1.2 | BLE central role (scanning, connecting, HID report subscriptions) |
| SSD1306 OLED driver | ^4.6.1 | Status display on the Heltec onboard OLED |

NimBLE is configured for central role only (`ROLE_PERIPHERAL=0`, `ROLE_BROADCASTER=0`) which saves ~200KB RAM compared to the full Bluedroid stack.

---

## Architecture

### Dual-Core Design

The ESP32-S3 has two Xtensa cores running at 240MHz. Each core has a dedicated role:

```
┌──────────────────────────────────────────────────────────────────┐
│  Core 0                              Core 1                     │
│  ┌──────────────────────┐            ┌────────────────────────┐ │
│  │ BLE Task (pri 3)     │            │ ADB Task (pri 5)       │ │
│  │  • NimBLE scan/conn  │            │  • Bit-banged bus loop │ │
│  │  • HID report parse  │──Queue──▶  │  • Keyboard emulation  │ │
│  │  • Reconnection mgmt │            │  • Mouse emulation     │ │
│  ├──────────────────────┤            │  • Interrupts disabled │ │
│  │ OLED Task (pri 1)    │            │    during bit I/O      │ │
│  │  • Status display    │            └────────────────────────┘ │
│  │  • 4Hz refresh       │            ┌────────────────────────┐ │
│  └──────────────────────┘            │ Arduino loop() (pri 1) │ │
│                                      │  • STATUS line output  │ │
│                                      └────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

- **Core 0** runs BLE (priority 3) and OLED (priority 1). BLE is the heavier workload — NimBLE callbacks, scanning, connection management, and HID report parsing all run here. OLED updates are cosmetic and lowest priority.
- **Core 1** runs the ADB bus loop (priority 5, highest in the system). This is timing-critical — ADB bit cells are 100us and interrupts are disabled during bit I/O. Arduino's `loop()` also runs here at priority 1, but only wakes every second to print diagnostics.

### Data Flow

```
BLE HID device (keyboard/mouse)
    │
    │  BLE notification (HID Report)
    ▼
on_keyboard_report() / on_mouse_report()    [Core 0, NimBLE callback]
    │
    │  Diff-based parsing: detect key press/release, mouse delta
    ▼
event_queue::send_kbd() / send_mouse()       [FreeRTOS queue, non-blocking]
    │
    │  Lock-free queue (xQueueSend)
    ▼
adb_keyboard::process_queue()                [Core 1, called from Talk handler]
adb_mouse::process_queue()
    │
    │  Ring buffer (keyboard) or delta accumulation (mouse)
    ▼
ADB Talk Register 0 response                [Core 1, bit-banged on GPIO48]
    │
    │  Open-drain, 100Ω series resistor
    ▼
Mac SE ADB bus
```

### Startup Sequence

Defined in `main.cpp::setup()`:

1. Serial init (115200 baud)
2. Event queues created (must be first — other modules push to them)
3. OLED display init (enables Vext power, resets SSD1306, shows splash)
4. ADB protocol init (configures GPIO48 as open-drain, inits keyboard/mouse state)
5. ADB self-test (optional, compile flag `ADB_SELF_TEST=1`)
6. BLE HID host init (NimBLE stack, starts scanning)
7. Bond clear check — if PRG button (GPIO0) is held, waits 3s with OLED countdown, then calls `NimBLEDevice::deleteAllBonds()`
8. FreeRTOS tasks pinned to cores:
   - Core 1: ADB bus loop (priority 5, 4KB stack)
   - Core 0: BLE task loop (priority 3, 8KB stack)
   - Core 0: OLED task loop (priority 1, 4KB stack)

---

## Source File Reference

```
ble-adb-bridge/
├── platformio.ini              Build config, dependencies, NimBLE flags
├── include/
│   ├── config.h                All pins, timing, queue sizes, compile flags
│   ├── adb_platform.h          GPIO HAL (drive_low, release, read_pin, timing)
│   ├── adb_protocol.h          ADB bus loop, bit I/O, command parsing
│   ├── adb_keyboard.h          Keyboard device emulation API
│   ├── adb_mouse.h             Mouse device emulation API
│   ├── ble_hid_host.h          BLE HID host API (scan, connect, status)
│   ├── event_queue.h           Inter-core queue types (KbdEvent, MouseEvent)
│   ├── keycode_map.h           USB-to-ADB keycode translation API
│   └── oled_display.h          OLED status display API
└── src/
    ├── main.cpp                Entry point, task creation, diagnostic loop
    ├── adb_platform.cpp        Direct GPIO register access (IRAM_ATTR)
    ├── adb_protocol.cpp        ADB bus loop, bit-level I/O, command dispatch
    ├── adb_keyboard.cpp        ADB keyboard device (addr 2), ring buffer
    ├── adb_mouse.cpp           ADB mouse device (addr 3), delta accumulation
    ├── ble_hid_host.cpp        BLE central: scan, connect, reconnect, parse HID
    ├── event_queue.cpp         FreeRTOS queue init and wrappers
    ├── keycode_map.cpp         256-entry USB→ADB lookup table
    └── oled_display.cpp        SSD1306 OLED status display
```

---

## ADB Protocol Implementation

### Platform Abstraction (`adb_platform`)

All functions are marked `IRAM_ATTR` to guarantee they run from RAM (no flash cache misses during timing-critical sections).

```cpp
adb_platform::init();                           // GPIO48 as OUTPUT_OPEN_DRAIN
adb_platform::drive_low();                      // Pull line low (GPIO.out1_w1tc)
adb_platform::release();                        // Release to high-Z (GPIO.out1_w1ts)
adb_platform::read_pin();                       // Read GPIO48 state (GPIO.in1.val)
adb_platform::micros_now();                     // esp_timer_get_time()
adb_platform::delay_us(us);                     // Tight busy-wait (no yield)
adb_platform::wait_for_state(state, timeout);   // Poll until pin matches state
adb_platform::measure_pulse(state, timeout);    // Measure pulse duration
adb_platform::interrupts_disable();             // portDISABLE_INTERRUPTS()
adb_platform::interrupts_enable();              // portENABLE_INTERRUPTS()
```

GPIO48 is in the ESP32-S3 upper GPIO bank (GPIOs 32-48), so the register bit offset is `48 - 32 = 16`. The bitmask `ADB_PIN_BITMASK` is precomputed in `config.h`.

### Bus Loop (`adb_protocol::bus_loop`)

The bus loop runs on Core 1 and never returns. It continuously monitors the ADB data line:

```
                    ┌──────────── 100µs bit cell ────────────┐
                    │                                        │
Bit '0':   ─────┐  │  65µs low      │     35µs high         │
                 └──────────────────┘                        │
                                                             │
Bit '1':   ─────┐  │  35µs low  │         65µs high         │
                 └──────────┘                                │
```

**Command reception sequence:**

1. Wait for line idle (high)
2. Detect falling edge — measure low pulse duration
3. If 560–1040us: valid attention pulse → measure sync high → receive 8-bit command
4. If >2800us: global reset → reinitialize both devices
5. Parse command byte: `[4-bit addr][2-bit cmd][2-bit register]`
6. Dispatch to keyboard (addr 2) or mouse (addr 3) handler

**Yield strategy (critical):**

The Mac SE polls keyboard (addr 2) then mouse (addr 3) back-to-back with only ~200us gap. A `vTaskDelay(1)` (minimum 1ms) between commands would consistently miss the mouse poll. Instead:

- Yield periodically every 256 iterations (~3 seconds at ~91 polls/sec)
- The 10ms idle-wait timeout (`wait_for_state(false, 10000)`) provides natural watchdog feeding during bus gaps

### Service Request (SRQ)

When the Mac polls one device, the *other* device can assert SRQ by extending the stop bit's low phase to 300us. This tells the Mac to poll the other device next, preventing starvation.

```cpp
// In handle_command():
bool other_has_data = is_kbd ? adb_mouse::has_data() : adb_keyboard::has_data();
consume_stop_bit(other_has_data);  // assert SRQ if other device has pending data
```

### Keyboard Emulation (`adb_keyboard`)

**Address:** 2 (default), **Handler ID:** 2 (Apple Extended Keyboard)

**Key event ring buffer:** 32-entry circular buffer stores ADB-formatted key events:
- Bit 7: release flag (1 = key up, 0 = key down)
- Bits 6:0: 7-bit ADB keycode

**Talk Register 0** returns up to 2 key events per poll:
```
[key1_with_release_flag] [key2_or_0xFF]
```

**Talk Register 2** holds modifier/LED state (active-low, 0xFFFF = all released):
```
Byte 0: [rsvd(4)][ScrollLock LED][CapsLock LED][rsvd(2)]
Byte 1: [Cmd][Opt][Shift][Ctrl][Reset/Pwr][CapsLock][Delete][rsvd]
```

**Talk Register 3** returns device info: `[0x60|addr][handler_id]`

**Listen Register 3** handles Mac address/handler enumeration during startup.

### Mouse Emulation (`adb_mouse`)

**Address:** 3 (default), **Handler ID:** 2 (standard 100 cpi)

**Delta accumulation:** Between ADB polls, incoming mouse movement deltas accumulate in `s_accum_dx` / `s_accum_dy`. Each Talk Register 0 response reports the accumulated delta (clamped to 7-bit signed, -64 to +63) and subtracts what was reported, carrying any remainder forward.

**Talk Register 0:**
```
Byte 0: [button][Y6..Y0]    button: 1=released, 0=pressed (inverted from USB)
Byte 1: [1][X6..X0]         bit 7 always 1 (2nd button released)
```

Returns false (no response) if no movement and no button change.

---

## BLE HID Host

### Connection States

```
DISCONNECTED ──scan──▶ CONNECTING ──▶ DISCOVERING ──▶ CONNECTED
       ▲                                                   │
       │                                              disconnect
       │                                                   │
       │◀──give up (10 attempts)── RECONNECTING ◀──────────┘
       │                               │    ▲
       │                               │    │ fail (exp. backoff)
       │                               └────┘
```

### Initial Connection (`try_connect`)

1. **Client acquisition** (prevents NimBLE client leak):
   ```
   getClientByPeerAddress(addr) → getDisconnectedClient() → createClient()
   ```
   NimBLE has a hard max of 3 client objects. This pattern reuses existing ones.

2. **Neutral callbacks** during setup — prevents corrupting keyboard/mouse state if the connection drops during service discovery.

3. **Service discovery** — `client->discoverAttributes()` enumerates all GATT services/characteristics.

4. **Device type detection** — checks for Boot Keyboard Input (0x2A22) or Boot Mouse Input (0x2A33) characteristics. Falls back to parsing the Report Map descriptor for Generic Desktop Usage (0x06 = keyboard, 0x02 = mouse).

5. **Encryption** — `client->secureConnection()` must be called before subscribing to HID characteristics. Without encryption, CCCD writes succeed but the device silently withholds notifications.

6. **Protocol mode** — Boot Protocol (0) for keyboards (clean 8-byte reports), Report Protocol for mice (trackpads often lack Boot Mouse Input).

7. **Subscription strategy:**
   - **Keyboard:** Boot KBD Input if Boot Protocol was set, else HID Report chars (with length filtering in the callback)
   - **Mouse:** First notifiable HID Report char (one is enough), else Boot Mouse Input as fallback

### Reconnection (`try_reconnect`)

When a bonded device disconnects (sleep, out of range), it enters `RECONNECTING` state:

- **Client preserved** — the NimBLE client object is kept (retains bond keys)
- **Exponential backoff** — 1s → 2s → 4s → ... → 30s cap
- **Max 10 attempts** — after which the device transitions to `DISCONNECTED` and falls back to scan-based discovery
- **Scan acceleration** — if a bonded device's advertisement appears during scanning, reconnect is triggered immediately (bypasses backoff timer)
- **Scan filter** — `BLE_HCI_SCAN_FILT_NO_WL_INITA` catches directed advertisements from bonded devices using resolvable private addresses (RPAs)
- **Fast re-encryption** — `secureConnection()` uses stored bond keys (no user interaction)
- **Type known** — `was_keyboard`/`was_mouse` flags saved at disconnect, so reconnection skips device type detection

### HID Report Parsing

**Keyboard reports** (8+ bytes: `[modifiers][reserved][key1..key6]`):

Diff-based — compares current report against `prev_keys[]` and `prev_modifiers`:
- Modifier changes: each USB modifier bit (Ctrl, Shift, Alt, Cmd) is checked independently
- Key releases: keys in prev_keys but not in current data
- Key presses: keys in current data but not in prev_keys

Reports shorter than 8 bytes are dropped (filters consumer/vendor reports from multi-characteristic devices like the NuPhy Air75).

**Mouse reports** (3+ bytes):

Two formats handled:
- **Report Protocol** (5-7 bytes): `[buttons][X_lo][X_hi][Y_lo][Y_hi][scroll_lo][scroll_hi]` — full 16-bit signed deltas passed through the queue
- **Boot Protocol** (3 bytes): `[buttons][dx_8bit][dy_8bit]` — 8-bit signed deltas

Deltas are **not** clamped at the BLE side. The ADB mouse accumulator handles clamping to 7-bit (-64 to +63) with carry-forward for any remainder.

---

## Inter-Core Communication

### Event Queues

FreeRTOS queues bridge Core 0 (BLE) and Core 1 (ADB) without locks:

```cpp
struct KbdEvent {
    uint8_t adb_keycode;   // 7-bit ADB keycode (already translated from USB)
    bool    released;       // true = key up
};

struct MouseEvent {
    int16_t dx, dy;        // signed deltas
    bool    button;         // true = left button pressed
};
```

| Queue | Size | Producer | Consumer |
|-------|------|----------|----------|
| Keyboard | 32 events | `on_keyboard_report` (Core 0) | `adb_keyboard::process_queue` (Core 1) |
| Mouse | 64 events | `on_mouse_report` (Core 0) | `adb_mouse::process_queue` (Core 1) |

All sends and receives are non-blocking (`timeout = 0`). Dropped events are silent — the diagnostic counters reveal if queues overflow.

The mouse queue is 64 (increased from 16) because at 1600 DPI, high-speed trackpad movement can generate bursts faster than ADB polling can drain.

---

## Keycode Translation

`keycode_map.cpp` contains a 256-entry lookup table mapping USB HID Usage Page 0x07 keycodes to ADB wire scan codes.

### Critical Distinction: ADB Wire Scan Codes vs Mac Virtual Keycodes

Apple's reference implementation (`Cosmo_USB2ADB.c`) maps USB keycodes to **Mac virtual keycodes**, which differ from ADB wire scan codes for some keys:

| Key | ADB Wire (correct) | Mac Virtual (wrong on wire) |
|-----|--------------------|-----------------------------|
| Right Arrow | 0x3C | 0x7C |
| Left Arrow | 0x3B | 0x7B |
| Down Arrow | 0x3D | 0x7D |
| Up Arrow | 0x3E | 0x7E |
| Right Shift | 0x7B | 0x3C |
| Right Option | 0x7C | 0x3D |
| Right Control | 0x7D | 0x3E |

Sending Mac virtual keycode 0x7E on the wire makes the Mac SE interpret Up Arrow as the Power key (shutdown dialog). Use [QEMU's `adb-keys.h`](https://github.com/qemu/qemu/blob/master/include/hw/input/adb-keys.h) as the authoritative reference.

### Modifier Handling

Modifiers are handled separately from regular keycodes. The `MODIFIER_MAP` table maps USB modifier bitmask positions to ADB keycodes:

| USB Bit | Modifier | ADB Keycode |
|---------|----------|-------------|
| 0x01 | Left Ctrl | 0x36 |
| 0x02 | Left Shift | 0x38 |
| 0x04 | Left Alt/Option | 0x3A |
| 0x08 | Left Cmd/GUI | 0x37 |
| 0x10 | Right Ctrl | 0x7D |
| 0x20 | Right Shift | 0x7B |
| 0x40 | Right Alt/Option | 0x7C |
| 0x80 | Right Cmd/GUI | 0x37 |

---

## OLED Display

The onboard SSD1306 (128x64) shows live status:

```
KBD: [OK] NuPhy Air75 V
MOU: [Rcon] Touch@LOFRE
ADB: ACTIVE  Rate:91/s
Polls:48210 Events:347
```

State labels: `---` (disconnected), `Scan`, `Conn`, `Disc`, `OK` (connected), `Rcon` (reconnecting).

A filled circle at the right edge blinks with ADB activity. Poll rate is calculated over 1-second intervals.

The display task runs at priority 1 on Core 0 — it never interferes with BLE or ADB.

`show_message(line1, line2)` is a blocking helper for init-time use (bond clear countdown, error messages). It draws centered text and returns immediately after sending to the display.

---

## Diagnostics and Debugging

### Serial STATUS Output (every 5 seconds)

```
[STATUS] KBD:OK MOU:OK adbPoll:48210 adbResp:347 kCb:120(used:120 drop:0) mCb:2560 mEvt:1014 heap:267000
[STATUS] kAge:45ms mAge:12ms kQ:0 mQ:1
[DIAG] KBD handles: h47=120
[DIAG] MOU handles: h28=2560
```

| Field | Meaning |
|-------|---------|
| `KBD`/`MOU` | `OK` = connected, `--` = not connected |
| `adbPoll` | Total ADB commands received from Mac |
| `adbResp` | Total Talk responses sent |
| `kCb` | Keyboard BLE callback invocations |
| `used`/`drop` | Keyboard reports accepted/rejected by length filter |
| `mCb` | Mouse BLE callback invocations |
| `mEvt` | Mouse events dequeued by ADB side |
| `heap` | Free heap bytes |
| `kAge`/`mAge` | Time since last BLE notification (ms) |
| `kQ`/`mQ` | Current queue depth |
| Handle stats | Which HID characteristic handles are firing and how often |

### What to Look For

- **`kAge` or `mAge` increasing while device shows connected** — BLE notifications stopped (encryption issue, subscription lost)
- **`drop` count growing** — non-keyboard HID Report chars firing (consumer/vendor reports, expected with NuPhy)
- **`mQ` consistently non-zero** — mouse events arriving faster than ADB can drain (increase `MOUSE_QUEUE_SIZE`)
- **`heap` decreasing over time** — memory leak (check NimBLE client creation/deletion)
- **`adbPoll` increasing but `adbResp` not** — ADB commands arriving but no data to report (normal when idle)
- **Handle stats showing unexpected handles** — helps identify which HID Report characteristic carries useful data

### Compile-Time Debug Flags

Set in `platformio.ini` `build_flags`:

| Flag | Effect |
|------|--------|
| `ADB_DEBUG_VERBOSE=1` | Log every ADB command, Talk response, key/modifier event |
| `ADB_SELF_TEST=1` | Run bit-timing self-test at boot (measures actual vs expected timing) |
| `ADB_BUS_MONITOR=1` | Passive mode — decode and log all bus traffic without emulating devices |

**Bus monitor mode** is useful for comparing the firmware's behavior against a real ADB keyboard connected to the Mac. It decodes commands and device responses without participating on the bus.

---

## Hardware Setup

### Pin Assignments

| Pin | Function |
|-----|----------|
| GPIO0 | PRG/BOOT button (active low, internal pull-up) — hold 3s at boot to clear BLE bonds |
| GPIO48 | ADB data (open-drain output) |
| GPIO36 | Vext control (LOW = power on OLED) |
| GPIO17 | OLED I2C SDA |
| GPIO18 | OLED I2C SCL |
| GPIO21 | OLED reset |

### ADB Wiring

```
ESP32 GPIO48 ──[100Ω]──┬── ADB data pin (Mac SE)
                        │
                       [1kΩ pull-up to +5V on Mac side]
```

The 100 ohm series resistor provides current limiting. The ADB bus has a 1K pull-up to +5V on the Mac side. GPIO48 is configured as open-drain — it can pull the line low but relies on the external pull-up to bring it high. No level shifter is used in the dev setup (the ESP32-S3 GPIO48 tolerates 5V input on this pin due to the series resistor limiting current into the ESD protection diodes).

### Tested Hardware

| Device | Role | BLE Characteristics |
|--------|------|---------------------|
| NuPhy Air75 V2 | Keyboard | 13 HID chars, 5 notifiable HID Report + Boot KBD Input |
| Lofree Touch | Mouse/trackpad | 7 HID chars, 2 notifiable HID Report + Boot Mouse Input |
| Macintosh SE | ADB host | Polls addr 2 (kbd) + addr 3 (mouse) at ~91 Hz |

---

## Configuration Reference

All constants live in `include/config.h`:

### ADB Timing

| Constant | Value | Notes |
|----------|-------|-------|
| `ADB_ATTN_MIN_US` | 560 | Minimum valid attention pulse |
| `ADB_ATTN_MAX_US` | 1040 | Maximum valid attention pulse |
| `ADB_BIT_CELL_US` | 100 | Total bit cell duration |
| `ADB_BIT_0_LOW_US` | 65 | '0' bit low phase |
| `ADB_BIT_1_LOW_US` | 35 | '1' bit low phase |
| `ADB_BIT_THRESHOLD_US` | 50 | <50us = '1', >=50us = '0' |
| `ADB_SRQ_LOW_US` | 300 | Service Request low duration |
| `ADB_TLT_US` | 200 | Stop-to-start time (device response delay) |
| `ADB_RESET_MIN_US` | 2800 | Global reset threshold |

### BLE

| Constant | Value | Notes |
|----------|-------|-------|
| `BLE_SCAN_DURATION_S` | 0 | Scan forever |
| `BLE_SCAN_INTERVAL_MS` | 100 | Scan interval |
| `BLE_SCAN_WINDOW_MS` | 80 | Scan window (must be <= interval) |

### Bond Clear

| Constant | Value | Notes |
|----------|-------|-------|
| `BOND_CLEAR_PIN` | 0 | GPIO0 (PRG/BOOT button on Heltec V3) |
| `BOND_CLEAR_HOLD_MS` | 3000 | Hold duration to confirm bond clear |

### BLE Reconnection

| Constant | Value | Notes |
|----------|-------|-------|
| `BLE_RECONNECT_TIMEOUT_MS` | 5000 | Per-attempt connect timeout |
| `BLE_RECONNECT_INITIAL_MS` | 1000 | Initial backoff delay |
| `BLE_RECONNECT_MAX_MS` | 30000 | Maximum backoff delay |
| `BLE_RECONNECT_MAX_ATTEMPTS` | 10 | Give up threshold |

### Queues and Tasks

| Constant | Value | Notes |
|----------|-------|-------|
| `KBD_QUEUE_SIZE` | 32 | Keyboard event queue depth |
| `MOUSE_QUEUE_SIZE` | 64 | Mouse event queue depth |
| `ADB_TASK_STACK_SIZE` | 4096 | ADB task stack (bytes) |
| `BLE_TASK_STACK_SIZE` | 8192 | BLE task stack (bytes) |
| `OLED_TASK_STACK_SIZE` | 4096 | OLED task stack (bytes) |
| `ADB_TASK_PRIORITY` | 5 | Highest — timing critical |
| `BLE_TASK_PRIORITY` | 3 | Mid — BLE management |
| `OLED_TASK_PRIORITY` | 1 | Lowest — cosmetic |

---

## Pitfalls and Hard-Won Lessons

These are issues that were debugged on real hardware. Future contributors should be aware of them.

### 1. Never Yield Inside the ADB Bus Loop Between Commands

The Mac SE polls keyboard (addr 2) then mouse (addr 3) back-to-back with only ~200us gap. A `vTaskDelay(1)` between commands costs 1ms minimum (one FreeRTOS tick) and consistently misses the second device's poll. The fix is to yield only periodically (every 256 iterations, ~3 seconds) or during the 10ms idle-wait timeout.

### 2. BLE HID Notifications Require Encryption

HID devices require encrypted connections before they'll send notifications. The insidious part: CCCD writes (subscribing to notifications) succeed over unencrypted links, but the device silently withholds all data. Debugging this looks like "subscription worked but no callbacks ever fire." Always call `client->secureConnection()` after service discovery, before subscribing.

### 3. Don't Require MITM for Mice

`setSecurityAuth(true, true, true)` requires MITM protection, which needs a display/keyboard for passkey entry. Mice and trackpads don't have displays — they can only do "Just Works" pairing. Setting MITM=true blocks pairing entirely with no useful error message. Use `setSecurityAuth(true, false, true)` (bonding + secure connections, no MITM).

### 4. Boot Protocol Silences Some Devices

Setting Boot Protocol mode (0) disables HID Report (0x2A4D) notifications. Some trackpads (like the Lofree Touch) lack a Boot Mouse Input characteristic — setting Boot Protocol silences everything. Use Boot Protocol for keyboards only, Report Protocol for mice.

### 5. ADB Wire Scan Codes vs Mac Virtual Keycodes

Apple's `Cosmo_USB2ADB.c` (commonly referenced online) maps USB keycodes to Mac virtual keycodes, not ADB wire scan codes. For most keys they're identical, but arrow keys and right modifiers are swapped. Sending virtual keycode 0x7E (Up Arrow) on the wire triggers the Power key (shutdown dialog). Use QEMU's `adb-keys.h` as the authoritative reference.

### 6. NimBLE Client Object Leak

`createClient()` on every connection without `deleteClient()` on disconnect hits NimBLE's max of 3 clients after a few connect/disconnect cycles. Always prefer reusing clients: `getClientByPeerAddress()` → `getDisconnectedClient()` → `createClient()`.

### 7. Mouse Queue Overflow at High DPI

At 1600 DPI, a trackpad generates mouse reports faster than ADB's ~91 Hz polling can drain them. The original queue size of 16 caused 60% event drops, making the cursor sluggish. Increased to 64.

### 8. Protocol Mode is Often Read-Only

Many BLE HID devices have a read-only Protocol Mode characteristic (the write property bit is 0). The write call is a no-op. The code checks `canWrite()` before attempting and handles the fallback gracefully.

### 9. Use Neutral Callbacks During Connection Setup

If the BLE connection drops during service discovery (before device type is known), the disconnect callback for the keyboard or mouse slot fires and corrupts that slot's state. Using neutral callbacks during the connection setup phase prevents this — the real callbacks are assigned only after the device type is determined and a slot is chosen.

### 10. Don't Clamp Mouse Deltas Before Queuing

BLE Report Protocol gives 16-bit signed deltas. `MouseEvent.dx`/`dy` are `int16_t`. Clamping to int8_t (-128 to +127) in `on_mouse_report` before queuing causes fast swipes (delta > 127 per BLE report) to lose movement — the cursor travels less than expected, feeling like lag. Pass the full 16-bit values through the queue. The ADB mouse accumulator already clamps to 7-bit (-64 to +63) with carry-forward for any remainder.
