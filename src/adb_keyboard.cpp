#include "adb_keyboard.h"
#include "event_queue.h"
#include "config.h"

#include <Arduino.h>

namespace adb_keyboard {

// ─── Internal state ─────────────────────────────────────────────────────────

static uint8_t s_address   = ADB_ADDR_KEYBOARD;  // current address (may change)
static uint8_t s_handler   = ADB_HANDLER_KEYBOARD;

// Key event ring buffer (holds ADB-formatted key events)
static constexpr int KEY_BUF_SIZE = 32;
static uint8_t s_key_buf[KEY_BUF_SIZE];  // each entry: [release_bit | 7-bit keycode]
static int s_key_head = 0;
static int s_key_tail = 0;

// Register 2: modifier key state (active-low bits)
// Bit 7: not used (1)
// Bit 6: not used (1)
// Bit 5: not used (1)
// Bit 4: not used (1)
// Bit 3: Scroll Lock LED (1=off)
// Bit 2: Caps Lock LED (1=off)
// Bit 1: not used (1)
// Bit 0: not used (1)
// Second byte: modifier key state
// Bit 7: Command
// Bit 6: Option
// Bit 5: Shift
// Bit 4: Control
// Bit 3: Reset/Power
// Bit 2: Caps Lock
// Bit 1: Delete
// Bit 0: not used (0)
static uint16_t s_register2 = 0xFFFF;  // all modifiers released

// ─── Buffer helpers ─────────────────────────────────────────────────────────

static bool buf_empty() {
    return s_key_head == s_key_tail;
}

static bool buf_full() {
    return ((s_key_head + 1) % KEY_BUF_SIZE) == s_key_tail;
}

static void buf_push(uint8_t key_event) {
    if (!buf_full()) {
        s_key_buf[s_key_head] = key_event;
        s_key_head = (s_key_head + 1) % KEY_BUF_SIZE;
    }
}

static uint8_t buf_pop() {
    uint8_t val = s_key_buf[s_key_tail];
    s_key_tail = (s_key_tail + 1) % KEY_BUF_SIZE;
    return val;
}

// ─── Public interface ───────────────────────────────────────────────────────

void init() {
    s_address = ADB_ADDR_KEYBOARD;
    s_handler = ADB_HANDLER_KEYBOARD;
    s_key_head = 0;
    s_key_tail = 0;
    s_register2 = 0xFFFF;
}

bool handle_talk(uint8_t reg, uint16_t& data) {
    switch (reg) {
        case 0: {
            // Register 0: key data — up to 2 key events
            process_queue();

            if (buf_empty()) return false;

            uint8_t key1 = buf_pop();
            uint8_t key2 = buf_empty() ? 0xFF : buf_pop();  // 0xFF = no second key

            data = ((uint16_t)key1 << 8) | key2;
            return true;
        }

        case 2:
            // Register 2: modifier key state + LED state
            data = s_register2;
            return true;

        case 3: {
            // Register 3: device info
            // Byte 0: [exceptional_event(0) | srq_enable(1) | 2-bit reserved(01) | 4-bit address]
            // Byte 1: handler ID
            uint8_t byte0 = 0x60 | (s_address & 0x0F);  // SRQ enabled, no exceptional event
            data = ((uint16_t)byte0 << 8) | s_handler;
            return true;
        }

        default:
            return false;
    }
}

void handle_listen(uint8_t reg, uint16_t data) {
    switch (reg) {
        case 2:
            // Host writing LED state / modifier state
            s_register2 = data;
            break;

        case 3: {
            // Host writing new address / handler ID (enumeration)
            uint8_t new_addr = data >> 8;
            uint8_t new_handler = data & 0xFF;

            // Address change request
            if (new_addr != 0 && new_addr != 0xFE) {
                s_address = new_addr & 0x0F;
#if ADB_DEBUG_VERBOSE
                Serial.printf("[KBD] Address changed to %d\n", s_address);
#endif
            }

            // Handler change
            if (new_handler != 0 && new_handler != 0xFE) {
                s_handler = new_handler;
#if ADB_DEBUG_VERBOSE
                Serial.printf("[KBD] Handler changed to %d\n", s_handler);
#endif
            }
            break;
        }

        default:
            break;
    }
}

void handle_flush() {
    s_key_head = 0;
    s_key_tail = 0;
}

void handle_reset() {
    init();
}

bool has_data() {
    return !buf_empty() || event_queue::kbd_pending();
}

uint8_t current_address() {
    return s_address;
}

void process_queue() {
    KbdEvent evt;
    while (event_queue::receive_kbd(evt)) {
        // Format: bit 7 = release flag, bits 6:0 = ADB keycode
        uint8_t adb_event = (evt.released ? 0x80 : 0x00) | (evt.adb_keycode & 0x7F);
        buf_push(adb_event);
    }
}

} // namespace adb_keyboard
