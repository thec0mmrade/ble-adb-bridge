#pragma once

#include <cstdint>

// ─── USB HID to ADB Keycode Translation ────────────────────────────────────
// 256-entry lookup table: USB HID Usage Page 0x07 → 7-bit ADB keycodes.
// Based on QuokkADB's usbtoadb.cpp mapping.

namespace keycode_map {

/// No valid ADB keycode (unmapped USB key).
constexpr uint8_t ADB_KEY_NONE = 0xFF;

/// Translate a USB HID keycode to an ADB keycode.
/// @param usb_keycode USB HID Usage Page 0x07 keycode (0x00–0xFF).
/// @return ADB 7-bit keycode, or ADB_KEY_NONE (0xFF) if unmapped.
uint8_t usb_to_adb(uint8_t usb_keycode);

// ─── USB HID modifier bit positions ────────────────────────────────────────
// These match the modifier byte in the USB HID boot protocol keyboard report.

constexpr uint8_t USB_MOD_LEFT_CTRL   = 0x01;
constexpr uint8_t USB_MOD_LEFT_SHIFT  = 0x02;
constexpr uint8_t USB_MOD_LEFT_ALT    = 0x04;
constexpr uint8_t USB_MOD_LEFT_GUI    = 0x08;
constexpr uint8_t USB_MOD_RIGHT_CTRL  = 0x10;
constexpr uint8_t USB_MOD_RIGHT_SHIFT = 0x20;
constexpr uint8_t USB_MOD_RIGHT_ALT   = 0x40;
constexpr uint8_t USB_MOD_RIGHT_GUI   = 0x80;

// ─── ADB modifier keycodes ─────────────────────────────────────────────────
constexpr uint8_t ADB_KEY_LEFT_CTRL   = 0x36;
constexpr uint8_t ADB_KEY_LEFT_SHIFT  = 0x38;
constexpr uint8_t ADB_KEY_LEFT_ALT    = 0x3A;  // Option
constexpr uint8_t ADB_KEY_LEFT_GUI    = 0x37;  // Command (Apple)
constexpr uint8_t ADB_KEY_RIGHT_CTRL  = 0x7D;  // ADB wire scan code (NOT 0x3E which is Up Arrow)
constexpr uint8_t ADB_KEY_RIGHT_SHIFT = 0x7B;  // ADB wire scan code (NOT 0x3C which is Right Arrow)
constexpr uint8_t ADB_KEY_RIGHT_ALT   = 0x7C;  // Option — ADB wire scan code (NOT 0x3D which is Down Arrow)
constexpr uint8_t ADB_KEY_RIGHT_GUI   = 0x37;  // Command (same as left on classic Mac)

/// Modifier bit-to-ADB-keycode mapping.
/// Index corresponds to bit position in USB modifier byte.
struct ModifierMapping {
    uint8_t usb_mask;
    uint8_t adb_keycode;
};

constexpr ModifierMapping MODIFIER_MAP[] = {
    { USB_MOD_LEFT_CTRL,   ADB_KEY_LEFT_CTRL   },
    { USB_MOD_LEFT_SHIFT,  ADB_KEY_LEFT_SHIFT  },
    { USB_MOD_LEFT_ALT,    ADB_KEY_LEFT_ALT    },
    { USB_MOD_LEFT_GUI,    ADB_KEY_LEFT_GUI    },
    { USB_MOD_RIGHT_CTRL,  ADB_KEY_RIGHT_CTRL  },
    { USB_MOD_RIGHT_SHIFT, ADB_KEY_RIGHT_SHIFT },
    { USB_MOD_RIGHT_ALT,   ADB_KEY_RIGHT_ALT   },
    { USB_MOD_RIGHT_GUI,   ADB_KEY_RIGHT_GUI   },
};

constexpr int MODIFIER_MAP_SIZE = sizeof(MODIFIER_MAP) / sizeof(MODIFIER_MAP[0]);

} // namespace keycode_map
