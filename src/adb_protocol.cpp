#include "adb_protocol.h"
#include "adb_platform.h"
#include "adb_keyboard.h"
#include "adb_mouse.h"
#include "oled_display.h"
#include "config.h"

#include <Arduino.h>

using namespace adb_platform;

namespace adb_protocol {

// ─── Low-level bit I/O ─────────────────────────────────────────────────────

void IRAM_ATTR send_bit(bool bit) {
    if (bit) {
        // '1' bit: 35µs low, 65µs high
        drive_low();
        delay_us(ADB_BIT_1_LOW_US);
        release();
        delay_us(ADB_BIT_1_HIGH_US);
    } else {
        // '0' bit: 65µs low, 35µs high
        drive_low();
        delay_us(ADB_BIT_0_LOW_US);
        release();
        delay_us(ADB_BIT_0_HIGH_US);
    }
}

void IRAM_ATTR send_byte(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        send_bit((byte >> i) & 1);
    }
}

void IRAM_ATTR send_data(uint16_t data) {
    // Start bit (always '1')
    send_bit(1);

    // 16 data bits, MSB first
    for (int i = 15; i >= 0; i--) {
        send_bit((data >> i) & 1);
    }

    // Stop bit (always '0')
    send_bit(0);
}

int IRAM_ATTR receive_bit() {
    // Wait for line to go low (start of bit cell)
    if (wait_for_state(false, ADB_BIT_CELL_US * 2) == 0) {
        return -1;  // timeout
    }

    // Measure low duration
    uint32_t low_time = measure_pulse(false, ADB_BIT_CELL_US + ADB_TIMING_TOLERANCE_US);
    if (low_time == 0) {
        return -1;
    }

    // Wait for line to go high (second half of bit cell)
    // Don't need to measure — just wait for it
    wait_for_state(true, ADB_BIT_CELL_US);

    // Decode: <50µs low = '1', >=50µs low = '0'
    return (low_time < ADB_BIT_THRESHOLD_US) ? 1 : 0;
}

int IRAM_ATTR receive_byte() {
    int result = 0;
    for (int i = 7; i >= 0; i--) {
        int bit = receive_bit();
        if (bit < 0) return -1;
        result |= (bit << i);
    }
    return result;
}

int32_t IRAM_ATTR receive_data() {
    // Wait for start bit
    int start = receive_bit();
    if (start < 0 || start != 1) {
        return -1;  // missing or invalid start bit
    }

    // Receive 16 data bits
    int32_t result = 0;
    for (int i = 15; i >= 0; i--) {
        int bit = receive_bit();
        if (bit < 0) return -1;
        result |= ((int32_t)bit << i);
    }

    // Receive stop bit (should be 0)
    receive_bit();  // consume it, don't validate strictly

    return result;
}

AdbCommand IRAM_ATTR receive_command() {
    AdbCommand cmd = {0, 0, 0, false};

    // Read the 8-bit command byte
    int byte = receive_byte();
    if (byte < 0) return cmd;

    // NOTE: We do NOT consume the stop bit here.
    // The stop bit is consumed by the caller so that SRQ can be asserted
    // during the stop bit low period if needed.

    // Parse: [4-bit addr][2-bit cmd][2-bit register]
    cmd.address = (byte >> 4) & 0x0F;
    cmd.command = (byte >> 2) & 0x03;
    cmd.reg     = byte & 0x03;
    cmd.valid   = true;

    return cmd;
}

void IRAM_ATTR assert_srq() {
    // SRQ: device holds the line low for 300µs total during the stop bit.
    // At this point the stop bit's low phase is in progress — we drive low
    // and extend it to the full SRQ duration.
    drive_low();
    delay_us(ADB_SRQ_LOW_US);
    release();
}

/// Consume the host's stop bit, optionally asserting SRQ.
void IRAM_ATTR consume_stop_bit(bool do_srq) {
    // The stop bit starts with a low phase (~65µs).
    // Wait for the line to go low (start of stop bit).
    wait_for_state(false, ADB_BIT_CELL_US * 2);

    if (do_srq) {
        // Assert SRQ by extending the low phase to 300µs
        assert_srq();
    } else {
        // Just wait for the stop bit to complete (line goes high)
        wait_for_state(true, ADB_BIT_CELL_US * 2);
    }
}

// ─── ADB activity counters ──────────────────────────────────────────────────
static volatile uint32_t s_poll_count = 0;
static volatile uint32_t s_talk_response_count = 0;

uint32_t get_poll_count()     { return s_poll_count; }
uint32_t get_response_count() { return s_talk_response_count; }

// ─── Bus monitoring ─────────────────────────────────────────────────────────

static void log_command(const AdbCommand& cmd) {
#if ADB_DEBUG_VERBOSE
    static const char* cmd_names[] = {"Reset", "Flush", "Listen", "Talk"};
    Serial.printf("[ADB] Addr:%d Cmd:%s Reg:%d\n",
                  cmd.address, cmd_names[cmd.command], cmd.reg);
#endif
}

// ─── Device dispatch ────────────────────────────────────────────────────────

/// Process a received ADB command. Called with the stop bit NOT yet consumed.
/// @param cmd Parsed command.
/// @param ints_disabled true if interrupts are currently disabled (caller must re-enable).
static void handle_command(const AdbCommand& cmd, bool ints_disabled) {
    oled_display::set_adb_active(true);
    oled_display::inc_poll_count();
    s_poll_count++;

    bool is_kbd   = (cmd.address == adb_keyboard::current_address());
    bool is_mouse = (cmd.address == adb_mouse::current_address());

    if (!is_kbd && !is_mouse) {
        // Not addressed to us — assert SRQ during the stop bit if we have data
        bool want_srq = adb_keyboard::has_data() || adb_mouse::has_data();
        consume_stop_bit(want_srq);
        if (ints_disabled) interrupts_enable();
        return;
    }

    // Addressed to us — assert SRQ if the OTHER device has pending data.
    // We emulate two devices on one bus, so when keyboard is polled,
    // mouse should signal if it needs attention, and vice versa.
    bool other_has_data = is_kbd ? adb_mouse::has_data() : adb_keyboard::has_data();
    consume_stop_bit(other_has_data);
    if (ints_disabled) interrupts_enable();

    switch (cmd.command) {
        case ADB_CMD_TALK: {
            uint16_t data;
            bool has_response = false;

            if (is_kbd) {
                has_response = adb_keyboard::handle_talk(cmd.reg, data);
            } else {
                has_response = adb_mouse::handle_talk(cmd.reg, data);
            }

            if (has_response) {
                // Wait Tlt (stop-to-start time)
                delay_us(ADB_TLT_US);

                interrupts_disable();
                send_data(data);
                interrupts_enable();

                oled_display::inc_event_count();

#if ADB_DEBUG_VERBOSE
                Serial.printf("[ADB] Talk A%d R%d -> 0x%04X\n", cmd.address, cmd.reg, data);
#endif
                s_talk_response_count++;
            }
            // No data = no response (bus stays idle per ADB spec)
            break;
        }

        case ADB_CMD_LISTEN: {
            // Wait for host to start sending data (falling edge of start bit)
            // The host controls Tlt timing — wait for the line to go low
            // rather than using a fixed delay
            if (wait_for_state(false, ADB_TLT_MAX_US + 100) == 0) {
                break;  // host didn't send data
            }

            interrupts_disable();
            int32_t data = receive_data();
            interrupts_enable();

            if (data >= 0) {
                if (is_kbd) {
                    adb_keyboard::handle_listen(cmd.reg, (uint16_t)data);
                } else {
                    adb_mouse::handle_listen(cmd.reg, (uint16_t)data);
                }
                Serial.printf("[ADB] Listen A%d R%d <- 0x%04X\n", cmd.address, cmd.reg, (uint16_t)data);
            }
            break;
        }

        case ADB_CMD_FLUSH:
            if (is_kbd) adb_keyboard::handle_flush();
            if (is_mouse) adb_mouse::handle_flush();
            Serial.printf("[ADB] Flush A%d\n", cmd.address);
            break;

        case ADB_CMD_RESET:
            if (is_kbd) adb_keyboard::handle_reset();
            if (is_mouse) adb_mouse::handle_reset();
            Serial.printf("[ADB] Reset A%d\n", cmd.address);
            break;
    }
}

// ─── Main bus loop ──────────────────────────────────────────────────────────

void init() {
    adb_platform::init();
    adb_keyboard::init();
    adb_mouse::init();
}

void bus_loop() {
    Serial.println("[ADB] Bus loop started on core " + String(xPortGetCoreID()));

    uint32_t yield_counter = 0;

    while (true) {
        // Always wait for line to be high (idle) first, then detect
        // the falling edge. This ensures we measure the full attention
        // pulse and don't catch a partial one already in progress.
        if (!read_pin()) {
            // Line is already low — we missed the start.
            // Wait for it to go high again before looking for next command.
            wait_for_state(true, ADB_RESET_MIN_US + 500);
            continue;
        }

        // Line is high (idle) — wait for falling edge (attention start)
        if (wait_for_state(false, 10000) == 0) {
            // No bus activity for 10ms — safe to yield for TWDT
            vTaskDelay(1);
            continue;
        }

        // Falling edge detected — measure the full low pulse duration
        uint32_t low_start = micros_now();
        wait_for_state(true, ADB_RESET_MIN_US + 500);
        uint32_t low_duration = micros_now() - low_start;

        if (low_duration >= ADB_RESET_MIN_US) {
            // Global reset — reset both devices to default addresses
            adb_keyboard::handle_reset();
            adb_mouse::handle_reset();
            Serial.printf("[ADB] Global reset (%luus)\n", low_duration);
            continue;
        }

        if (low_duration >= ADB_ATTN_MIN_US && low_duration <= ADB_ATTN_MAX_US) {
            // Valid attention pulse — line is now high (sync period)
            // Measure sync high duration
            uint32_t sync_start = micros_now();
            wait_for_state(false, ADB_SYNC_NOMINAL_US + 30);
            uint32_t sync = micros_now() - sync_start;

            if (sync > 0) {
                // Read command byte (line just went low = start of first bit)
                // Keep interrupts disabled through stop bit consumption
                // (handle_command will re-enable them)
                interrupts_disable();
                AdbCommand cmd = receive_command();

                if (cmd.valid) {
                    handle_command(cmd, true);  // true = ints still disabled
                    log_command(cmd);
                } else {
                    interrupts_enable();
                }
            }
        }
        // else: noise or invalid pulse — ignore, loop back to wait for idle

        // Periodic yield to keep Core 1 idle task alive for TWDT.
        // Yields ~1ms every 256 iterations (~3s at 91 polls/s).
        // We must NOT yield after every command — the Mac sends
        // back-to-back polls (keyboard then mouse) with only ~200µs
        // gap, and a 1ms delay would consistently miss the mouse.
        if (++yield_counter >= 256) {
            yield_counter = 0;
            vTaskDelay(1);
        }
    }
}

void self_test() {
    Serial.println("[ADB] === Timing Self-Test ===");

    // Test bit transmission timing
    const int NUM_TESTS = 10;

    Serial.println("[ADB] Testing '1' bit timing (expect ~35µs low, ~65µs high):");
    for (int i = 0; i < NUM_TESTS; i++) {
        uint32_t start = micros_now();
        interrupts_disable();
        drive_low();
        delay_us(ADB_BIT_1_LOW_US);
        uint32_t mid = micros_now();
        release();
        delay_us(ADB_BIT_1_HIGH_US);
        uint32_t end = micros_now();
        interrupts_enable();

        Serial.printf("  [%d] low=%luµs high=%luµs total=%luµs\n",
                      i, mid - start, end - mid, end - start);
    }

    Serial.println("[ADB] Testing '0' bit timing (expect ~65µs low, ~35µs high):");
    for (int i = 0; i < NUM_TESTS; i++) {
        uint32_t start = micros_now();
        interrupts_disable();
        drive_low();
        delay_us(ADB_BIT_0_LOW_US);
        uint32_t mid = micros_now();
        release();
        delay_us(ADB_BIT_0_HIGH_US);
        uint32_t end = micros_now();
        interrupts_enable();

        Serial.printf("  [%d] low=%luµs high=%luµs total=%luµs\n",
                      i, mid - start, end - mid, end - start);
    }

    // Loopback test: transmit a known byte and read it back
    // (only works if the pin is wired to itself or has a pull-up)
    Serial.println("[ADB] Loopback test (line state after release):");
    release();
    delay_us(100);
    bool idle_state = read_pin();
    Serial.printf("  Idle state: %s (expect HIGH)\n", idle_state ? "HIGH" : "LOW");

    drive_low();
    delay_us(50);
    bool driven_state = read_pin();
    Serial.printf("  Driven low: %s (expect LOW)\n", driven_state ? "HIGH" : "LOW");

    release();
    delay_us(50);
    bool released_state = read_pin();
    Serial.printf("  Released:   %s (expect HIGH)\n", released_state ? "HIGH" : "LOW");

    Serial.println("[ADB] === Self-Test Complete ===");
}

void bus_monitor() {
    Serial.println("[ADB] === Bus Monitor Mode ===");
    Serial.println("[ADB] Passively listening to ADB bus traffic...");

    while (true) {
        // Wait for line to go low
        if (!read_pin()) {
            uint32_t low_duration = measure_pulse(false, ADB_RESET_MIN_US + 500);

            if (low_duration >= ADB_RESET_MIN_US) {
                Serial.printf("[MON] Global Reset (%luµs)\n", low_duration);
                wait_for_state(true, 5000);
                continue;
            }

            if (low_duration >= ADB_ATTN_MIN_US && low_duration <= ADB_ATTN_MAX_US) {
                // Read sync
                uint32_t sync = measure_pulse(true, 200);
                Serial.printf("[MON] Attn=%luµs Sync=%luµs ", low_duration, sync);

                // Read command byte
                int cmd_byte = receive_byte();
                if (cmd_byte >= 0) {
                    int addr = (cmd_byte >> 4) & 0x0F;
                    int cmd  = (cmd_byte >> 2) & 0x03;
                    int reg  = cmd_byte & 0x03;
                    static const char* cmd_names[] = {"Reset", "Flush", "Listen", "Talk"};
                    Serial.printf("Cmd=0x%02X [Addr:%d %s R%d]",
                                  cmd_byte, addr, cmd_names[cmd], reg);

                    // Consume stop bit
                    receive_bit();

                    // If Talk, try to read device response
                    if (cmd == ADB_CMD_TALK) {
                        // Wait for response (Tlt + start bit)
                        uint32_t wait_start = micros_now();
                        bool got_response = false;

                        while ((micros_now() - wait_start) < 500) {
                            if (!read_pin()) {
                                // Device is responding
                                int32_t data = receive_data();
                                if (data >= 0) {
                                    Serial.printf(" → 0x%04X", (uint16_t)data);
                                    got_response = true;
                                }
                                break;
                            }
                        }
                        if (!got_response) {
                            Serial.print(" (no response)");
                        }
                    }

                    // If Listen, try to read host data
                    if (cmd == ADB_CMD_LISTEN) {
                        delay_us(ADB_TLT_US);
                        int32_t data = receive_data();
                        if (data >= 0) {
                            Serial.printf(" ← 0x%04X", (uint16_t)data);
                        }
                    }
                }
                Serial.println();
            }
        }
        vTaskDelay(1);
    }
}

} // namespace adb_protocol
