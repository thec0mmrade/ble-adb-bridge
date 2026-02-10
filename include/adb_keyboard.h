#pragma once

#include <cstdint>

// ─── ADB Keyboard Device Emulation (Address 2) ─────────────────────────────
// Emulates a standard Apple ADB keyboard. Responds to Talk/Listen/Flush/Reset
// commands from the Mac host. Key events arrive via FreeRTOS queue from BLE.

namespace adb_keyboard {

/// Initialize the keyboard device state.
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

/// Handle a Flush command — clear all pending key events.
void handle_flush();

/// Handle a Reset command — reset to default state.
void handle_reset();

/// Check if the keyboard has pending data (for SRQ).
bool has_data();

/// Get the current ADB address (may change during enumeration).
uint8_t current_address();

/// Process incoming events from the BLE queue.
/// Call periodically from the ADB task to transfer events from the
/// FreeRTOS queue into the internal key event buffer.
void process_queue();

} // namespace adb_keyboard
