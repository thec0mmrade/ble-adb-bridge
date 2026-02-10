#pragma once

#include <cstdint>

// ─── BLE HID Host (NimBLE Central) ─────────────────────────────────────────
// Scans for BLE HID devices (keyboard and mouse), connects, subscribes to
// HID Report notifications, parses reports, and pushes events to queues.

namespace ble_hid_host {

/// Device connection state.
enum class DeviceState : uint8_t {
    DISCONNECTED,
    SCANNING,
    CONNECTING,
    DISCOVERING,
    CONNECTED,
    RECONNECTING
};

/// Status of a connected BLE device.
struct DeviceStatus {
    DeviceState state;
    char name[32];
    bool is_keyboard;
    bool is_mouse;
};

/// Initialize the NimBLE stack and start scanning for HID devices.
void init();

/// Main BLE task loop — runs on Core 0.
/// Handles scanning, connection management, and reconnection.
/// This function never returns.
void task_loop();

/// Get the current keyboard device status.
DeviceStatus get_keyboard_status();

/// Get the current mouse device status.
DeviceStatus get_mouse_status();

/// Check if a keyboard is connected.
bool keyboard_connected();

/// Check if a mouse is connected.
bool mouse_connected();

/// Get BLE mouse callback invocation count (diagnostic).
uint32_t get_mouse_cb_count();

/// Get BLE keyboard callback invocation count (diagnostic).
uint32_t get_kbd_cb_count();

/// Get count of keyboard reports that passed length filter.
uint32_t get_kbd_cb_used();

/// Get count of keyboard reports rejected by length filter.
uint32_t get_kbd_cb_dropped();

/// Get millis() timestamp of last keyboard notification.
uint32_t get_kbd_last_ms();

/// Get millis() timestamp of last mouse notification.
uint32_t get_mouse_last_ms();

/// Print per-handle callback stats to Serial.
void dump_handle_stats();

} // namespace ble_hid_host
