#include "adb_platform.h"
#include "config.h"

#include <Arduino.h>
#include <soc/gpio_struct.h>
#include <esp_timer.h>

// GPIO48 is in the upper bank (GPIOs 32-48).
// Register offset bit = 48 - 32 = 16.
// We use GPIO.out1_w1ts / GPIO.out1_w1tc / GPIO.in1.val for direct access.

namespace adb_platform {

void init() {
    // Configure GPIO48 as open-drain output with internal pull-up disabled
    // (external 1kΩ pull-up is used on the ADB side of the level shifter)
    pinMode(ADB_DATA_PIN, OUTPUT_OPEN_DRAIN);
    release();  // start with line released (high via pull-up)
}

void IRAM_ATTR drive_low() {
    // Set output low — actively pulls the line down
    GPIO.out1_w1tc.val = ADB_PIN_BITMASK;
}

void IRAM_ATTR release() {
    // Set output high — open-drain means high-Z, pull-up brings line high
    GPIO.out1_w1ts.val = ADB_PIN_BITMASK;
}

bool IRAM_ATTR read_pin() {
    // Enable input on the pin (ESP32-S3 needs input enabled to read)
    return (GPIO.in1.val & ADB_PIN_BITMASK) != 0;
}

uint32_t IRAM_ATTR micros_now() {
    return (uint32_t)esp_timer_get_time();
}

void IRAM_ATTR delay_us(uint32_t us) {
    uint32_t start = micros_now();
    while ((micros_now() - start) < us) {
        // tight spin — no yield, no NOP needed at 240MHz
    }
}

uint32_t IRAM_ATTR wait_for_state(bool state, uint32_t timeout_us) {
    uint32_t start = micros_now();
    while (read_pin() != state) {
        uint32_t elapsed = micros_now() - start;
        if (elapsed >= timeout_us) {
            return 0;  // timed out
        }
    }
    return micros_now() - start;
}

uint32_t IRAM_ATTR measure_pulse(bool state, uint32_t timeout_us) {
    // Verify the line is currently in the expected state
    if (read_pin() != state) {
        return 0;
    }

    uint32_t start = micros_now();
    while (read_pin() == state) {
        uint32_t elapsed = micros_now() - start;
        if (elapsed >= timeout_us) {
            return elapsed;  // still in state at timeout
        }
    }
    return micros_now() - start;
}

void IRAM_ATTR interrupts_disable() {
    portDISABLE_INTERRUPTS();
}

void IRAM_ATTR interrupts_enable() {
    portENABLE_INTERRUPTS();
}

} // namespace adb_platform
