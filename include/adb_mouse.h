#pragma once

#include <cstdint>

// ─── ADB Mouse Device Emulation (Address 3) ────────────────────────────────
// Emulates a standard Apple ADB mouse (100 cpi, 1 button).
// Mouse deltas accumulate between ADB polls and are clamped to 7-bit range.

namespace adb_mouse {

/// Initialize the mouse device state.
void init();

/// Handle a Talk command for the given register.
/// @param reg Register number (0-3).
/// @param data Output: 16-bit data to send to host.
/// @return true if there is data to send, false if no response.
bool handle_talk(uint8_t reg, uint16_t& data);

/// Handle a Listen command — host is writing data to us.
/// @param reg Register number (0-3).
/// @param data 16-bit data received from host.
void handle_listen(uint8_t reg, uint16_t data);

/// Handle a Flush command — clear accumulated deltas.
void handle_flush();

/// Handle a Reset command — reset to default state.
void handle_reset();

/// Check if the mouse has pending data (movement or button change).
bool has_data();

/// Get the current ADB address (may change during enumeration).
uint8_t current_address();

/// Process incoming events from the BLE queue.
/// Accumulates deltas for the next Talk Register 0 response.
void process_queue();

/// Get total mouse events dequeued (diagnostic).
uint32_t get_queue_events();

} // namespace adb_mouse
