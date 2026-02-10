#pragma once

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ─── Event Types ────────────────────────────────────────────────────────────

/// Keyboard event: a single key press or release.
struct KbdEvent {
    uint8_t adb_keycode;   // 7-bit ADB keycode (0x00–0x7F)
    bool    released;      // true = key released, false = key pressed
};

/// Mouse event: button state + movement deltas.
struct MouseEvent {
    int16_t dx;            // X movement (signed, will be clamped to 7-bit)
    int16_t dy;            // Y movement (signed, will be clamped to 7-bit)
    bool    button;        // true = button pressed (will be inverted for ADB)
};

// ─── Queue Interface ────────────────────────────────────────────────────────

namespace event_queue {

/// Initialize both keyboard and mouse event queues.
/// Must be called before any other queue operations.
void init();

/// Get the keyboard event queue handle.
QueueHandle_t kbd_queue();

/// Get the mouse event queue handle.
QueueHandle_t mouse_queue();

/// Push a keyboard event (non-blocking). Returns true on success.
bool send_kbd(const KbdEvent& evt);

/// Push a mouse event (non-blocking). Returns true on success.
bool send_mouse(const MouseEvent& evt);

/// Pop a keyboard event (non-blocking). Returns true if an event was available.
bool receive_kbd(KbdEvent& evt);

/// Pop a mouse event (non-blocking). Returns true if an event was available.
bool receive_mouse(MouseEvent& evt);

/// Check if keyboard queue has pending events.
bool kbd_pending();

/// Check if mouse queue has pending events.
bool mouse_pending();

} // namespace event_queue
