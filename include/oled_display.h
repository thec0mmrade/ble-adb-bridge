#pragma once

#include <cstdint>

// ─── OLED Display ───────────────────────────────────────────────────────────
// Status display on Heltec V3's onboard 128x64 SSD1306 OLED.
// Non-blocking updates at 4Hz on Core 0.

namespace oled_display {

/// Initialize the OLED display (I2C, reset pin).
void init();

/// Update the display with current status.
/// Non-blocking — skips if called too frequently.
void update();

/// Main display task loop — runs on Core 0.
/// Calls update() at the configured interval.
/// This function never returns.
void task_loop();

/// Set the ADB bus activity indicator.
void set_adb_active(bool active);

/// Increment the ADB poll counter.
void inc_poll_count();

/// Increment the event counter (key/mouse events processed).
void inc_event_count();

/// Show a centered message on the display (blocking, for init-time use).
void show_message(const char* line1, const char* line2 = nullptr);

} // namespace oled_display
