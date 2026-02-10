#pragma once

#include <cstdint>

// ─── ADB Protocol Engine ───────────────────────────────────────────────────
// Device-side ADB implementation. The Mac SE is the host — it polls.
// The ESP32 listens on the bus and responds as keyboard (addr 2) and
// mouse (addr 3).

namespace adb_protocol {

/// Parsed ADB command from the host.
struct AdbCommand {
    uint8_t address;    // 4-bit device address (0-15)
    uint8_t command;    // 2-bit command (Reset/Flush/Listen/Talk)
    uint8_t reg;        // 2-bit register number (0-3)
    bool    valid;      // true if command was successfully parsed
};

/// Initialize the ADB protocol engine.
void init();

/// Main ADB bus loop — runs on Core 1.
/// Waits for host attention, decodes commands, dispatches to device handlers.
/// This function never returns.
void bus_loop();

/// Run a timing self-test: transmit and verify bit timing.
/// Logs results to serial. Call before entering bus_loop.
void self_test();

/// Passive bus monitor mode — decode and log all bus traffic.
/// Used for debugging with a real Mac + real keyboard.
/// This function never returns.
void bus_monitor();

// ─── Low-level bit I/O (IRAM_ATTR in .cpp) ─────────────────────────────

/// Send a single ADB bit (device→host).
void send_bit(bool bit);

/// Send a byte as 8 ADB bits, MSB first.
void send_byte(uint8_t byte);

/// Send a 16-bit data word with start bit and stop bit (Talk response).
void send_data(uint16_t data);

/// Receive a single ADB bit from the bus.
/// @return -1 on timeout/error, 0 or 1 for the bit value.
int receive_bit();

/// Receive 8 bits from the bus (MSB first).
/// @return -1 on error, 0-255 for the byte value.
int receive_byte();

/// Receive 16-bit data word (for Listen commands).
/// @return -1 on error, 0-65535 for the data value.
int32_t receive_data();

/// Wait for and decode an attention + sync + command sequence.
/// @return Parsed AdbCommand (check .valid).
AdbCommand receive_command();

/// Assert a Service Request (extend stop-bit low to 300µs).
void assert_srq();

/// Get total ADB polls received.
uint32_t get_poll_count();

/// Get total Talk responses sent.
uint32_t get_response_count();

} // namespace adb_protocol
