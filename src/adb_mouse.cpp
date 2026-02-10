#include "adb_mouse.h"
#include "event_queue.h"
#include "config.h"

#include <Arduino.h>

namespace adb_mouse {

// ─── Internal state ─────────────────────────────────────────────────────────

static uint8_t s_address = ADB_ADDR_MOUSE;
static uint8_t s_handler = ADB_HANDLER_MOUSE;

// Accumulated movement deltas (signed, accumulate between polls)
static int16_t s_accum_dx = 0;
static int16_t s_accum_dy = 0;

// Button state: ADB uses 1=released, 0=pressed (inverted from USB)
static bool s_button_pressed = false;
static bool s_button_changed = false;

// ─── Helpers ────────────────────────────────────────────────────────────────

/// Clamp a value to 7-bit signed range (-64 to +63).
static int8_t clamp7(int16_t val) {
    if (val > 63) return 63;
    if (val < -64) return -64;
    return (int8_t)val;
}

// ─── Public interface ───────────────────────────────────────────────────────

void init() {
    s_address = ADB_ADDR_MOUSE;
    s_handler = ADB_HANDLER_MOUSE;
    s_accum_dx = 0;
    s_accum_dy = 0;
    s_button_pressed = false;
    s_button_changed = false;
}

bool handle_talk(uint8_t reg, uint16_t& data) {
    switch (reg) {
        case 0: {
            // Register 0: mouse data
            // [button(1=up)][7-bit Y delta][1][7-bit X delta]
            process_queue();

            if (s_accum_dx == 0 && s_accum_dy == 0 && !s_button_changed) {
                return false;  // no movement, no button change
            }

            // Clamp deltas to 7-bit signed range
            int8_t dx = clamp7(s_accum_dx);
            int8_t dy = clamp7(s_accum_dy);

            // Subtract what we're reporting (remainder carries forward)
            s_accum_dx -= dx;
            s_accum_dy -= dy;
            s_button_changed = false;

            // Pack into ADB mouse format:
            // Byte 0: [button][Y6..Y0]  — button: 1=released, 0=pressed
            // Byte 1: [1][X6..X0]       — bit 7 always 1 (reserved / 2nd button released)
            uint8_t button_bit = s_button_pressed ? 0x00 : 0x80;  // invert for ADB
            uint8_t byte0 = button_bit | (dy & 0x7F);
            uint8_t byte1 = 0x80 | (dx & 0x7F);  // bit 7 = 1 (2nd button released)

            data = ((uint16_t)byte0 << 8) | byte1;
            return true;
        }

        case 3: {
            // Register 3: device info
            uint8_t byte0 = 0x60 | (s_address & 0x0F);  // SRQ enabled
            data = ((uint16_t)byte0 << 8) | s_handler;
            return true;
        }

        default:
            return false;
    }
}

void handle_listen(uint8_t reg, uint16_t data) {
    if (reg == 3) {
        // Address / handler change (enumeration)
        uint8_t new_addr = data >> 8;
        uint8_t new_handler = data & 0xFF;

        if (new_addr != 0 && new_addr != 0xFE) {
            s_address = new_addr & 0x0F;
#if ADB_DEBUG_VERBOSE
            Serial.printf("[MOUSE] Address changed to %d\n", s_address);
#endif
        }
        if (new_handler != 0 && new_handler != 0xFE) {
            s_handler = new_handler;
#if ADB_DEBUG_VERBOSE
            Serial.printf("[MOUSE] Handler changed to %d\n", s_handler);
#endif
        }
    }
}

void handle_flush() {
    s_accum_dx = 0;
    s_accum_dy = 0;
    s_button_changed = false;
}

void handle_reset() {
    init();
}

bool has_data() {
    return (s_accum_dx != 0) || (s_accum_dy != 0) || s_button_changed
           || event_queue::mouse_pending();
}

uint8_t current_address() {
    return s_address;
}

static volatile uint32_t s_events_from_queue = 0;

void process_queue() {
    MouseEvent evt;
    while (event_queue::receive_mouse(evt)) {
        s_accum_dx += evt.dx;
        s_accum_dy += evt.dy;
        s_events_from_queue++;

        bool new_button = evt.button;
        if (new_button != s_button_pressed) {
            s_button_pressed = new_button;
            s_button_changed = true;
        }
    }
}

uint32_t get_queue_events() { return s_events_from_queue; }

} // namespace adb_mouse
