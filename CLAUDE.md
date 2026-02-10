# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
pio run -t upload        # compile and flash
pio device monitor       # serial monitor (115200 baud)
pio run                  # compile only
```

## Architecture

ESP32-S3 (Heltec V3) dual-core firmware bridging BLE HID to Apple Desktop Bus.

- **Core 0**: BLE scanning/connection (NimBLE-Arduino 2.3.7), HID report parsing, OLED display
- **Core 1**: ADB bus loop — bit-banged, timing-critical, interrupts disabled during bit I/O
- **Bridge**: FreeRTOS queues (`KbdEvent`, `MouseEvent`) connect cores lock-free

### Data Flow
```
BLE HID report → on_keyboard_report/on_mouse_report (Core 0)
  → event_queue::send_kbd/send_mouse (FreeRTOS queue)
  → adb_keyboard::process_queue / adb_mouse::process_queue (Core 1, called from Talk handler)
  → ADB Talk Register 0 response on the wire
```

### Key Files
| File | Role |
|------|------|
| `src/ble_hid_host.cpp` | BLE Central: scan, connect, reconnect bonded devices, detect type, parse HID reports |
| `src/adb_protocol.cpp` | ADB bus loop, attention detection, command dispatch |
| `src/adb_keyboard.cpp` | Keyboard device emulation (address 2), key event ring buffer |
| `src/adb_mouse.cpp` | Mouse device emulation (address 3), delta accumulation |
| `src/event_queue.cpp` | FreeRTOS queue wrappers |
| `src/keycode_map.cpp` | 256-entry USB-to-ADB keycode lookup table |
| `include/config.h` | All pins, timing constants, queue sizes, compile flags |

### Startup Sequence (`main.cpp::setup()`)

1. Serial init (115200 baud)
2. Event queues created (must be first — other modules push to them)
3. OLED display init (enables Vext power, resets SSD1306, shows splash)
4. ADB protocol init (configures GPIO48 as open-drain, inits keyboard/mouse state)
5. ADB self-test (optional, `ADB_SELF_TEST=1`)
6. BLE HID host init (NimBLE stack, starts scanning)
7. Bond clear check — if PRG button (GPIO0) held 3s, calls `NimBLEDevice::deleteAllBonds()`
8. FreeRTOS tasks pinned: ADB bus loop on Core 1 (pri 5, 4KB), BLE task on Core 0 (pri 3, 8KB), OLED on Core 0 (pri 1, 4KB)

## Compile-Time Flags

Override in `platformio.ini` `build_flags`:
- `ADB_DEBUG_VERBOSE=1` — log every ADB command and BLE event
- `ADB_SELF_TEST=1` — run bit-timing self-test at boot
- `ADB_BUS_MONITOR=1` — passive bus monitor mode (no device emulation)

## Key Constraints & Lessons

### ADB Bus Timing (Critical)
- Mac SE polls devices back-to-back with only ~200µs gap (keyboard addr 2, then mouse addr 3)
- **Never** add `vTaskDelay()` inside the bus loop between commands — even 1ms (one FreeRTOS tick) consistently misses the second device's poll
- Use periodic yield (every 256 iterations, ~3s) for TWDT; the 10ms idle-wait timeout provides natural watchdog feeding during bus gaps

### BLE HID Security
- HID devices **require encrypted connections** for notifications to flow
- CCCD writes succeed over unencrypted links but the device silently withholds notifications — this is deceptive to debug
- Must call `client->secureConnection()` after service discovery, before subscribing
- `setSecurityAuth(true, true, true)` blocks "Just Works" pairing (no MITM possible for mice without displays)
- Use `setSecurityAuth(true, false, true)` — bonding + secure connections, no MITM requirement

### BLE Protocol Mode
- Boot Protocol mode (0) disables HID Report (0x2A4D) notifications
- Some trackpads lack Boot Mouse Input (0x2A33) — setting Boot Protocol silences everything
- **Boot Protocol for keyboards only**, Report Protocol for mice
- Many devices have read-only Protocol Mode characteristic (W=0) — the write is a no-op but the code handles it gracefully

### ADB Keycodes: Scan Codes vs Virtual Keycodes (Critical)
- Apple's `Cosmo_USB2ADB.c` maps USB keycodes to **Mac virtual keycodes**, NOT ADB wire scan codes
- For most keys they're identical, but **arrow keys and right modifiers were swapped**:
  - Arrow keys: ADB wire=`0x3B`–`0x3E`, Mac virtual=`0x7B`–`0x7E`
  - Right modifiers (Shift/Alt/Ctrl): ADB wire=`0x7B`–`0x7D`, Mac virtual=`0x3C`–`0x3E`
- Sending virtual keycode `0x7E` on the wire makes the Mac SE interpret Up Arrow as the Power key (shutdown dialog)
- **Use QEMU's `adb-keys.h`** as the authoritative reference for ADB wire scan codes, not `Cosmo_USB2ADB.c`
- Full audit completed against QEMU: F-keys (F1–F15) and navigation keys (Home/End/PgUp/PgDn/Help/FwdDel) are all correct
- All keycodes verified working on Mac SE after fix

### BLE Reconnection
- Bonded devices (keyboard/mouse) that disconnect enter `RECONNECTING` state instead of `DISCONNECTED`
- `try_reconnect()` reuses the existing NimBLE client object (preserves bond keys for fast re-encryption)
- Exponential backoff: 1s → 2s → 4s → ... → 30s cap, gives up after 10 attempts → falls back to `DISCONNECTED` (scan-based discovery)
- If a bonded device appears in scan results during `RECONNECTING`, triggers immediate reconnect (bypasses backoff timer)
- Scan filter policy `BLE_HCI_SCAN_FILT_NO_WL_INITA` detects directed advertisements from bonded devices using resolvable private addresses
- Client reuse pattern in `try_connect()`: `getClientByPeerAddress()` → `getDisconnectedClient()` → `createClient()` (prevents client object leak at NimBLE's max of 3)
- `onDisconnect` preserves `was_keyboard`/`was_mouse` flags so `try_reconnect()` skips device type detection and resubscribes to the correct HID characteristics directly
- Reconnection constants in `config.h`: `BLE_RECONNECT_TIMEOUT_MS` (5s), `BLE_RECONNECT_INITIAL_MS` (1s), `BLE_RECONNECT_MAX_MS` (30s), `BLE_RECONNECT_MAX_ATTEMPTS` (10)

### NimBLE Client Object Leak Prevention
- `createClient()` without cleanup hits NimBLE's max of 3 clients after a few connect/disconnect cycles
- Always reuse clients: `getClientByPeerAddress()` → `getDisconnectedClient()` → `createClient()`
- Use **neutral callbacks** during connection setup — if the connection drops during service discovery (before device type is known), the real keyboard/mouse disconnect callback would corrupt that slot's state

### Mouse Report Format
- Lofree Touch uses 7-byte Report Protocol: `[buttons][X_lo][X_hi][Y_lo][Y_hi][scroll_lo][scroll_hi]`
- Reports come on HID Report characteristic (0x2A4D), not Boot Mouse Input (0x2A33)
- Code handles both Boot Protocol (3 bytes) and Report Protocol (5-7 bytes) formats
- **Full 16-bit deltas** are passed through the queue to `adb_mouse` — do NOT clamp to int8_t in `on_mouse_report`. The ADB side clamps to 7-bit with carry-forward.

### Bond Clear (BOOT Button)
- Hold the **PRG button** (GPIO0) for 3 seconds during startup to erase all BLE bonds
- Check runs in `setup()` after `ble_hid_host::init()` (NimBLE bond store must be initialized)
- OLED shows countdown; serial logs bond count before/after
- After clearing, all devices must re-pair from scratch
- Constants in `config.h`: `BOND_CLEAR_PIN` (0), `BOND_CLEAR_HOLD_MS` (3000)

### NuPhy Air75 V2 HID Characteristics
- 5 notifiable HID Report (0x2A4D) chars + Boot Keyboard Input (0x2A22) — all 6 get subscribed
- Not all HID Report chars carry keyboard data; some are consumer/vendor reports
- Keyboard reports should be 8 bytes: `[modifiers][reserved][key1..key6]`
- Reports shorter than 8 bytes are filtered by `if (length < 8) return;`

## Diagnostic Serial Output

The STATUS lines (every 5s) show:
```
[STATUS] KBD:OK MOU:OK adbPoll:48210 adbResp:347 kCb:120(used:120 drop:0) mCb:2560 mEvt:1014 heap:267000
[STATUS] kAge:45ms mAge:12ms kQ:0 mQ:1
```
- `KBD`/`MOU`: BLE connection state (`OK` or `--`)
- `adbPoll`: total ADB commands received from Mac
- `adbResp`: total Talk responses sent
- `kCb`: keyboard BLE callback count; `used`/`drop`: reports accepted/rejected by length filter
- `mCb`: mouse BLE callback count
- `mEvt`: mouse events dequeued by ADB side
- `heap`: free heap bytes
- `kAge`/`mAge`: time since last BLE notification (ms) — increasing while connected means notifications stopped
- `kQ`/`mQ`: current queue depth — `mQ` consistently non-zero means mouse events arriving faster than ADB can drain

## Known Issues (Active)

- NuPhy has 5 notifiable HID Report chars; not all carry keyboard data — consumer/vendor reports are filtered by length check.
- `if (length < 8) return;` may discard non-standard keyboard reports from other devices.
- OLED shows "Rcon" during reconnection; STATUS line shows "--" (uses separate formatting path).

## Tested Hardware
- **Keyboard**: NuPhy Air75 V2 (BLE, 13 HID characteristics)
- **Mouse**: Lofree Touch trackpad (BLE, 7 HID characteristics)
- **ADB Host**: Macintosh SE
- **Dev wiring**: GPIO48 with 100Ω series resistor (no level shifter)
