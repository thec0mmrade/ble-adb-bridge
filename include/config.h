#pragma once

#include <cstdint>

// ─── GPIO ───────────────────────────────────────────────────────────────────
// GPIO48 — free on Heltec V3, no boot-strap or peripheral conflicts
// Upper GPIO bank: bit offset = 48 - 32 = 16
constexpr int ADB_DATA_PIN = 48;
constexpr uint32_t ADB_PIN_BITMASK = (1UL << (ADB_DATA_PIN - 32));

// ─── Vext (Heltec V3 external power control) ────────────────────────────────
// GPIO36 controls power to OLED and other external peripherals.
// LOW = power on, HIGH = power off.
constexpr int VEXT_PIN = 36;

// ─── OLED (Heltec V3 onboard SSD1306 128x64) ───────────────────────────────
constexpr int OLED_SDA = 17;
constexpr int OLED_SCL = 18;
constexpr int OLED_RST = 21;
constexpr int OLED_ADDR = 0x3C;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;

// ─── ADB Protocol Timing (microseconds) ────────────────────────────────────
// Reference: Apple ADB spec, lopaciuk.eu

// Attention signal from host
constexpr uint32_t ADB_ATTN_MIN_US       = 560;    // min attention duration
constexpr uint32_t ADB_ATTN_MAX_US       = 1040;   // max attention duration
constexpr uint32_t ADB_ATTN_NOMINAL_US   = 800;    // typical attention

// Sync signal (high after attention)
constexpr uint32_t ADB_SYNC_MIN_US       = 50;     // min sync high
constexpr uint32_t ADB_SYNC_NOMINAL_US   = 65;     // typical sync high

// Bit cell timing
constexpr uint32_t ADB_BIT_CELL_US       = 100;    // total bit cell
constexpr uint32_t ADB_BIT_0_LOW_US      = 65;     // '0' bit: 65µs low, 35µs high
constexpr uint32_t ADB_BIT_0_HIGH_US     = 35;     // '0' bit: 35µs high
constexpr uint32_t ADB_BIT_1_LOW_US      = 35;     // '1' bit: 35µs low, 65µs high
constexpr uint32_t ADB_BIT_1_HIGH_US     = 65;     // '1' bit: 65µs high
constexpr uint32_t ADB_BIT_THRESHOLD_US  = 50;     // <50µs low = '1', >=50µs low = '0'

// Stop bit
constexpr uint32_t ADB_STOP_LOW_US       = 65;     // stop bit low (same as '0')
constexpr uint32_t ADB_STOP_HIGH_MIN_US  = 35;     // minimum stop bit high

// Service Request (SRQ) — device extends stop-bit low
constexpr uint32_t ADB_SRQ_LOW_US        = 300;    // SRQ: hold low for 300µs

// Device response timing
constexpr uint32_t ADB_TLT_US            = 200;    // Stop-to-Start time (Tlt)
constexpr uint32_t ADB_TLT_MAX_US        = 260;    // max Tlt before host gives up

// Global reset
constexpr uint32_t ADB_RESET_MIN_US      = 2800;   // >2800µs low = global reset

// Timing tolerance
constexpr uint32_t ADB_TIMING_TOLERANCE_US = 15;   // ±15µs tolerance on bit reads

// ─── ADB Addresses ─────────────────────────────────────────────────────────
constexpr uint8_t ADB_ADDR_KEYBOARD      = 2;      // default keyboard address
constexpr uint8_t ADB_ADDR_MOUSE         = 3;      // default mouse address

// ─── ADB Commands (2-bit) ──────────────────────────────────────────────────
constexpr uint8_t ADB_CMD_RESET          = 0;      // 00 — Reset
constexpr uint8_t ADB_CMD_FLUSH          = 1;      // 01 — Flush
constexpr uint8_t ADB_CMD_LISTEN         = 2;      // 10 — Listen (host→device)
constexpr uint8_t ADB_CMD_TALK           = 3;      // 11 — Talk (device→host)

// ─── ADB Handler IDs ───────────────────────────────────────────────────────
constexpr uint8_t ADB_HANDLER_KEYBOARD   = 2;      // Apple Extended Keyboard handler
constexpr uint8_t ADB_HANDLER_MOUSE      = 2;      // Standard 100cpi mouse handler (not 4 — that's extended)

// ─── Event Queue Sizes ─────────────────────────────────────────────────────
constexpr int KBD_QUEUE_SIZE             = 32;     // keyboard event queue depth
constexpr int MOUSE_QUEUE_SIZE           = 64;     // mouse event queue depth

// ─── BLE ────────────────────────────────────────────────────────────────────
constexpr uint32_t BLE_SCAN_DURATION_S   = 0;      // 0 = scan forever
constexpr uint32_t BLE_SCAN_INTERVAL_MS  = 100;    // scan interval
constexpr uint32_t BLE_SCAN_WINDOW_MS    = 80;     // scan window (must be <= interval)

// ─── Bond Clear Button ──────────────────────────────────────────────────────
constexpr int      BOND_CLEAR_PIN     = 0;     // GPIO0 (BOOT button on Heltec V3)
constexpr uint32_t BOND_CLEAR_HOLD_MS = 3000;  // hold 3 seconds to clear bonds

// ─── BLE Reconnection ───────────────────────────────────────────────────────
constexpr uint32_t BLE_RECONNECT_TIMEOUT_MS    = 5000;   // connect timeout per attempt
constexpr uint32_t BLE_RECONNECT_INITIAL_MS    = 1000;   // initial backoff delay
constexpr uint32_t BLE_RECONNECT_MAX_MS        = 30000;  // max backoff delay
constexpr int      BLE_RECONNECT_MAX_ATTEMPTS  = 10;     // give up after this many failures

// ─── OLED Update ────────────────────────────────────────────────────────────
constexpr uint32_t OLED_UPDATE_INTERVAL_MS = 250;  // 4 Hz display refresh

// ─── Debug ──────────────────────────────────────────────────────────────────
constexpr int SERIAL_BAUD               = 115200;

// Compile-time flags (override via build_flags in platformio.ini)
#ifndef ADB_DEBUG_VERBOSE
#define ADB_DEBUG_VERBOSE 0      // 1 = log every ADB command/response
#endif

#ifndef ADB_SELF_TEST
#define ADB_SELF_TEST 0          // 1 = run timing self-test at boot
#endif

#ifndef ADB_BUS_MONITOR
#define ADB_BUS_MONITOR 0        // 1 = passive bus monitor mode (no device emulation)
#endif

// ─── Task Stack Sizes ───────────────────────────────────────────────────────
constexpr uint32_t ADB_TASK_STACK_SIZE   = 4096;
constexpr uint32_t BLE_TASK_STACK_SIZE   = 8192;
constexpr uint32_t OLED_TASK_STACK_SIZE  = 4096;

// ─── Task Priorities ────────────────────────────────────────────────────────
constexpr int ADB_TASK_PRIORITY          = 5;      // highest — timing-critical
constexpr int BLE_TASK_PRIORITY          = 3;
constexpr int OLED_TASK_PRIORITY         = 1;      // lowest — cosmetic only
