#pragma once

#include <cstdint>

// ─── ADB Platform HAL ──────────────────────────────────────────────────────
// Direct GPIO register access and microsecond timing for ADB bit-banging
// on ESP32-S3 (Heltec V3). All functions are IRAM_ATTR in the .cpp for
// flash-cache safety during interrupt-disabled sections.

namespace adb_platform {

/// Initialize GPIO48 as open-collector output (open-drain mode).
void init();

/// Drive the ADB data line low (active pull-down).
void drive_low();

/// Release the ADB data line (high-Z — pull-up resistor brings it high).
void release();

/// Read the current state of the ADB data line.
/// @return true if line is high, false if low.
bool read_pin();

/// Get current time in microseconds (hardware timer, no syscall overhead).
uint32_t micros_now();

/// Busy-wait for a specified number of microseconds.
/// Uses esp_timer hardware counter — tight loop, no yielding.
void delay_us(uint32_t us);

/// Wait for the ADB data line to reach a specific state.
/// @param state true = wait for high, false = wait for low.
/// @param timeout_us Maximum time to wait.
/// @return Elapsed time in µs, or 0 if timed out.
uint32_t wait_for_state(bool state, uint32_t timeout_us);

/// Measure how long the line stays in a given state.
/// @param state true = measure high duration, false = measure low duration.
/// @param timeout_us Maximum time to measure.
/// @return Duration in µs, or 0 if line was not in the expected state initially.
uint32_t measure_pulse(bool state, uint32_t timeout_us);

/// Disable interrupts on current core (for timing-critical ADB I/O).
void interrupts_disable();

/// Re-enable interrupts on current core.
void interrupts_enable();

} // namespace adb_platform
