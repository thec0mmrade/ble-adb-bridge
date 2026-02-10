#include "oled_display.h"
#include "ble_hid_host.h"
#include "config.h"

#include <Arduino.h>
#include <Wire.h>
#include <SSD1306Wire.h>

namespace oled_display {

// ─── State ──────────────────────────────────────────────────────────────────

static SSD1306Wire* s_display = nullptr;
static uint32_t s_last_update = 0;
static bool     s_adb_active  = false;
static uint32_t s_poll_count  = 0;
static uint32_t s_event_count = 0;
static uint32_t s_last_poll_count = 0;
static uint32_t s_last_rate_time  = 0;
static float    s_poll_rate       = 0;

// ─── Helpers ────────────────────────────────────────────────────────────────

static const char* state_str(ble_hid_host::DeviceState state) {
    switch (state) {
        case ble_hid_host::DeviceState::DISCONNECTED: return "---";
        case ble_hid_host::DeviceState::SCANNING:     return "Scan";
        case ble_hid_host::DeviceState::CONNECTING:   return "Conn";
        case ble_hid_host::DeviceState::DISCOVERING:  return "Disc";
        case ble_hid_host::DeviceState::CONNECTED:    return "OK";
        case ble_hid_host::DeviceState::RECONNECTING: return "Rcon";
        default: return "?";
    }
}

// ─── Public interface ───────────────────────────────────────────────────────

void init() {
    // Enable Vext — powers the OLED on Heltec V3
    pinMode(VEXT_PIN, OUTPUT);
    digitalWrite(VEXT_PIN, LOW);  // LOW = power on
    delay(100);

    // Reset OLED via reset pin
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);
    delay(50);

    s_display = new SSD1306Wire(OLED_ADDR, OLED_SDA, OLED_SCL);
    s_display->init();
    s_display->flipScreenVertically();
    s_display->setFont(ArialMT_Plain_10);

    // Splash screen
    s_display->clear();
    s_display->setTextAlignment(TEXT_ALIGN_CENTER);
    s_display->drawString(64, 10, "BLE-ADB Bridge");
    s_display->drawString(64, 30, "Heltec V3");
    s_display->drawString(64, 45, "Initializing...");
    s_display->display();

    s_last_update = millis();
    s_last_rate_time = millis();
}

void update() {
    uint32_t now = millis();

    // Calculate poll rate (polls per second)
    uint32_t dt = now - s_last_rate_time;
    if (dt >= 1000) {
        s_poll_rate = (float)(s_poll_count - s_last_poll_count) * 1000.0f / dt;
        s_last_poll_count = s_poll_count;
        s_last_rate_time = now;
    }

    auto kbd_status = ble_hid_host::get_keyboard_status();
    auto mouse_status = ble_hid_host::get_mouse_status();

    s_display->clear();
    s_display->setTextAlignment(TEXT_ALIGN_LEFT);

    // Line 1: Keyboard status
    char line[64];
    snprintf(line, sizeof(line), "KBD: [%s] %.16s",
             state_str(kbd_status.state), kbd_status.name);
    s_display->drawString(0, 0, line);

    // Line 2: Mouse status
    snprintf(line, sizeof(line), "MOU: [%s] %.16s",
             state_str(mouse_status.state), mouse_status.name);
    s_display->drawString(0, 14, line);

    // Line 3: ADB bus status
    snprintf(line, sizeof(line), "ADB: %s  Rate:%.0f/s",
             s_adb_active ? "ACTIVE" : "idle", s_poll_rate);
    s_display->drawString(0, 28, line);

    // Line 4: Counters
    snprintf(line, sizeof(line), "Polls:%lu Events:%lu",
             s_poll_count, s_event_count);
    s_display->drawString(0, 42, line);

    // Activity indicator (small filled circle when ADB is active)
    if (s_adb_active) {
        s_display->fillCircle(122, 32, 4);
        s_adb_active = false;  // auto-clear, must be set each cycle
    }

    s_display->display();
}

void task_loop() {
    Serial.println("[OLED] Task loop started on core " + String(xPortGetCoreID()));

    while (true) {
        update();
        vTaskDelay(pdMS_TO_TICKS(OLED_UPDATE_INTERVAL_MS));
    }
}

void set_adb_active(bool active) {
    s_adb_active = active;
}

void inc_poll_count() {
    s_poll_count++;
}

void inc_event_count() {
    s_event_count++;
}

void show_message(const char* line1, const char* line2) {
    if (!s_display) return;
    s_display->clear();
    s_display->setTextAlignment(TEXT_ALIGN_CENTER);
    s_display->drawString(64, line2 ? 16 : 24, line1);
    if (line2) {
        s_display->drawString(64, 36, line2);
    }
    s_display->display();
    s_display->setTextAlignment(TEXT_ALIGN_LEFT);  // restore for update()
}

} // namespace oled_display
