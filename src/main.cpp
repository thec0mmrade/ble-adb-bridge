#include <Arduino.h>
#include <NimBLEDevice.h>
#include "config.h"
#include "event_queue.h"
#include "adb_protocol.h"
#include "ble_hid_host.h"
#include "adb_mouse.h"
#include "oled_display.h"

// ─── Task Handles ───────────────────────────────────────────────────────────
static TaskHandle_t s_adb_task  = nullptr;
static TaskHandle_t s_ble_task  = nullptr;
static TaskHandle_t s_oled_task = nullptr;

// ─── Task Functions ─────────────────────────────────────────────────────────

/// ADB protocol loop — runs on Core 1 (timing-critical).
/// Listens for host commands on the ADB bus and responds as keyboard/mouse.
static void adb_task_func(void* param) {
#if ADB_BUS_MONITOR
    adb_protocol::bus_monitor();
#else
    adb_protocol::bus_loop();
#endif
    // Never reaches here
}

/// BLE HID host loop — runs on Core 0.
/// Scans for and connects to BLE keyboards and mice.
static void ble_task_func(void* param) {
    ble_hid_host::task_loop();
    // Never reaches here
}

/// OLED display loop — runs on Core 0.
/// Updates the status display at 4Hz.
static void oled_task_func(void* param) {
    oled_display::task_loop();
    // Never reaches here
}

// ─── Arduino entry points ───────────────────────────────────────────────────

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(1000);  // wait for serial monitor

    Serial.println();
    Serial.println("=================================");
    Serial.println("  BLE-to-ADB Bridge");
    Serial.println("  Heltec WiFi LoRa 32 V3");
    Serial.println("=================================");
    Serial.printf("  CPU: %d MHz\n", getCpuFrequencyMhz());
    Serial.printf("  Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("  ADB pin: GPIO%d\n", ADB_DATA_PIN);
    Serial.println();

    // ─── Initialize modules ─────────────────────────────────────────────

    // 1. Event queues (must be first — other modules push to them)
    Serial.println("[INIT] Creating event queues...");
    event_queue::init();

    // 2. OLED display (includes Vext power-on)
    Serial.println("[INIT] Initializing OLED...");
    oled_display::init();

    // 3. ADB protocol (GPIO, device state)
    Serial.println("[INIT] Initializing ADB protocol...");
    adb_protocol::init();

#if ADB_SELF_TEST
    // 4. Run ADB timing self-test
    adb_protocol::self_test();
#endif

    // 5. BLE HID host (NimBLE)
    Serial.println("[INIT] Initializing BLE...");
    ble_hid_host::init();

    // 6. Bond clear check — hold BOOT button at startup to erase all BLE bonds
    pinMode(BOND_CLEAR_PIN, INPUT_PULLUP);
    if (digitalRead(BOND_CLEAR_PIN) == LOW) {
        int num_bonds = NimBLEDevice::getNumBonds();
        Serial.printf("[INIT] BOOT button held — hold 3s to clear bonds... (%d bonded)\n", num_bonds);

        uint32_t start = millis();
        bool held = true;
        while (millis() - start < BOND_CLEAR_HOLD_MS) {
            uint32_t remaining_ms = BOND_CLEAR_HOLD_MS - (millis() - start);
            char msg[32];
            snprintf(msg, sizeof(msg), "%d.%ds remaining...", (int)(remaining_ms / 1000), (int)(remaining_ms % 1000 / 100));
            oled_display::show_message("Hold BOOT 3s", msg);

            if (digitalRead(BOND_CLEAR_PIN) == HIGH) {
                held = false;
                break;
            }
            delay(100);
        }

        if (held) {
            NimBLEDevice::deleteAllBonds();
            Serial.printf("[INIT] Bonds cleared! (was: %d bonded devices)\n", num_bonds);
            oled_display::show_message("Bonds cleared!", nullptr);
            delay(1500);
        } else {
            Serial.println("[INIT] BOOT button released early — bonds kept");
        }
    }

    // ─── Pin tasks to cores ─────────────────────────────────────────────

    Serial.println("[INIT] Starting tasks...");

    // Core 1: ADB bus loop (timing-critical, highest priority)
    xTaskCreatePinnedToCore(
        adb_task_func,
        "ADB",
        ADB_TASK_STACK_SIZE,
        nullptr,
        ADB_TASK_PRIORITY,
        &s_adb_task,
        1  // Core 1
    );

    // Core 0: BLE HID host
    xTaskCreatePinnedToCore(
        ble_task_func,
        "BLE",
        BLE_TASK_STACK_SIZE,
        nullptr,
        BLE_TASK_PRIORITY,
        &s_ble_task,
        0  // Core 0
    );

    // Core 0: OLED display (lowest priority)
    xTaskCreatePinnedToCore(
        oled_task_func,
        "OLED",
        OLED_TASK_STACK_SIZE,
        nullptr,
        OLED_TASK_PRIORITY,
        &s_oled_task,
        0  // Core 0
    );

    Serial.println("[INIT] All tasks started");
    Serial.printf("[INIT] Free heap after init: %d bytes\n", ESP.getFreeHeap());
    Serial.println();
}

void loop() {
    // All work is done in FreeRTOS tasks.
    // Arduino loop() runs on Core 1 at priority 1, below ADB task.
    // Use it for periodic serial status output.

    static uint32_t last_status = 0;
    uint32_t now = millis();

    if ((now - last_status) >= 5000) {
        last_status = now;

        uint32_t kbd_age = ble_hid_host::get_kbd_last_ms() ?
            (now - ble_hid_host::get_kbd_last_ms()) : 0;
        uint32_t mou_age = ble_hid_host::get_mouse_last_ms() ?
            (now - ble_hid_host::get_mouse_last_ms()) : 0;

        Serial.printf("[STATUS] KBD:%s MOU:%s adbPoll:%lu adbResp:%lu kCb:%lu(used:%lu drop:%lu) mCb:%lu mEvt:%lu heap:%d\n",
                      ble_hid_host::keyboard_connected() ? "OK" : "--",
                      ble_hid_host::mouse_connected() ? "OK" : "--",
                      adb_protocol::get_poll_count(),
                      adb_protocol::get_response_count(),
                      ble_hid_host::get_kbd_cb_count(),
                      ble_hid_host::get_kbd_cb_used(),
                      ble_hid_host::get_kbd_cb_dropped(),
                      ble_hid_host::get_mouse_cb_count(),
                      adb_mouse::get_queue_events(),
                      ESP.getFreeHeap());
        Serial.printf("[STATUS] kAge:%lums mAge:%lums kQ:%d mQ:%d\n",
                      kbd_age, mou_age,
                      uxQueueMessagesWaiting(event_queue::kbd_queue()),
                      uxQueueMessagesWaiting(event_queue::mouse_queue()));
        ble_hid_host::dump_handle_stats();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
