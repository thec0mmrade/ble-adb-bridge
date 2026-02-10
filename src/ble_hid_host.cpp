#include "ble_hid_host.h"
#include "event_queue.h"
#include "keycode_map.h"
#include "config.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <algorithm>

namespace ble_hid_host {

// ─── UUIDs ──────────────────────────────────────────────────────────────────
static const NimBLEUUID HID_SERVICE_UUID("1812");
static const NimBLEUUID HID_REPORT_UUID("2A4D");
static const NimBLEUUID BOOT_KBD_INPUT_UUID("2A22");
static const NimBLEUUID BOOT_MOUSE_INPUT_UUID("2A33");
static const NimBLEUUID REPORT_MAP_UUID("2A4B");

// ─── Device tracking ────────────────────────────────────────────────────────

struct BleDevice {
    NimBLEClient* client = nullptr;
    DeviceStatus  status = { DeviceState::DISCONNECTED, {0}, false, false };
    uint8_t       prev_keys[6] = {0};
    uint8_t       prev_modifiers = 0;
    bool          prev_buttons = false;

    // Reconnection state
    NimBLEAddress bonded_addr;
    bool          was_keyboard = false;
    bool          was_mouse = false;
    uint32_t      reconnect_next_ms = 0;
    uint32_t      reconnect_delay_ms = 0;
    int           reconnect_attempts = 0;
};

static BleDevice s_keyboard;
static BleDevice s_mouse;
static bool s_scanning = false;

// Pending connection: scan callback stores address, task_loop connects
static NimBLEAddress s_pending_addr;
static char          s_pending_name[32] = {0};
static bool          s_pending_connect = false;

// ─── Forward declarations ───────────────────────────────────────────────────
static void on_keyboard_report(NimBLERemoteCharacteristic* chr,
                               uint8_t* data, size_t length, bool is_notify);
static void on_mouse_report(NimBLERemoteCharacteristic* chr,
                            uint8_t* data, size_t length, bool is_notify);
static void start_scan();
static bool try_connect(const NimBLEAddress& addr, const char* name);
static bool try_reconnect(BleDevice* device, const char* label);
static void handle_reconnection(BleDevice* device, const char* label);

// ─── Client callbacks ───────────────────────────────────────────────────────

class ClientCallbacks : public NimBLEClientCallbacks {
public:
    BleDevice* device;
    const char* label;

    ClientCallbacks(BleDevice* dev, const char* lbl) : device(dev), label(lbl) {}

    void onConnect(NimBLEClient* client) override {
        Serial.printf("[BLE] [%s] Connected to %s\n",
                      label, client->getPeerAddress().toString().c_str());
        device->status.state = DeviceState::DISCOVERING;
    }

    void onDisconnect(NimBLEClient* client, int reason) override {
        Serial.printf("[BLE] [%s] Disconnected from %s (reason=%d)\n",
                      label, device->status.name, reason);

        // Clear input state
        memset(device->prev_keys, 0, sizeof(device->prev_keys));
        device->prev_modifiers = 0;
        device->prev_buttons = false;

        // If we had a known device type, enter RECONNECTING — keep client & address
        if (device->status.is_keyboard || device->status.is_mouse) {
            device->was_keyboard = device->status.is_keyboard;
            device->was_mouse = device->status.is_mouse;
            device->bonded_addr = client->getPeerAddress();
            device->reconnect_delay_ms = BLE_RECONNECT_INITIAL_MS;
            device->reconnect_next_ms = millis() + BLE_RECONNECT_INITIAL_MS;
            device->reconnect_attempts = 0;
            device->status.state = DeviceState::RECONNECTING;
            // Keep device->client — reused by try_reconnect()
            Serial.printf("[BLE] [%s] Will reconnect to %s (backoff %lums)\n",
                          label, device->bonded_addr.toString().c_str(),
                          device->reconnect_delay_ms);
        } else {
            // Never fully connected — clean up
            device->status.state = DeviceState::DISCONNECTED;
            device->status.is_keyboard = false;
            device->status.is_mouse = false;
            if (device->client) {
                NimBLEDevice::deleteClient(device->client);
            }
            device->client = nullptr;
        }
    }
};

static ClientCallbacks s_kbd_callbacks(&s_keyboard, "KBD");
static ClientCallbacks s_mouse_callbacks(&s_mouse, "MOU");

/// Neutral callbacks used during initial connection before device type is known.
/// Prevents corrupting keyboard/mouse state during the connect phase.
class NeutralCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* client) override {
        Serial.printf("[BLE] [INIT] Connected to %s\n",
                      client->getPeerAddress().toString().c_str());
    }
    void onDisconnect(NimBLEClient* client, int reason) override {
        Serial.printf("[BLE] [INIT] Disconnected during setup (reason=%d)\n", reason);
    }
};
static NeutralCallbacks s_neutral_callbacks;

// ─── Scan callbacks ─────────────────────────────────────────────────────────

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        // Check if this is a bonded device we're trying to reconnect to
        NimBLEAddress adv_addr = device->getAddress();
        if (s_keyboard.status.state == DeviceState::RECONNECTING &&
            s_keyboard.bonded_addr == adv_addr) {
            Serial.printf("[BLE] [KBD] Bonded device seen in scan — triggering immediate reconnect\n");
            s_keyboard.reconnect_next_ms = millis();  // trigger now
            NimBLEDevice::getScan()->stop();
            s_scanning = false;
            return;
        }
        if (s_mouse.status.state == DeviceState::RECONNECTING &&
            s_mouse.bonded_addr == adv_addr) {
            Serial.printf("[BLE] [MOU] Bonded device seen in scan — triggering immediate reconnect\n");
            s_mouse.reconnect_next_ms = millis();  // trigger now
            NimBLEDevice::getScan()->stop();
            s_scanning = false;
            return;
        }

        if (!device->isAdvertisingService(HID_SERVICE_UUID)) {
            return;
        }

        // Don't connect if we already have a pending connection
        if (s_pending_connect) return;

        bool need_kbd   = (s_keyboard.status.state == DeviceState::DISCONNECTED);
        bool need_mouse = (s_mouse.status.state == DeviceState::DISCONNECTED);
        if (!need_kbd && !need_mouse) {
            NimBLEDevice::getScan()->stop();
            s_scanning = false;
            return;
        }

        // Save address and name — connection happens in task_loop
        s_pending_addr = device->getAddress();
        strncpy(s_pending_name, device->getName().c_str(), sizeof(s_pending_name) - 1);
        s_pending_name[sizeof(s_pending_name) - 1] = '\0';
        s_pending_connect = true;

        Serial.printf("[BLE] Found HID device: %s (%s)\n",
                      s_pending_name,
                      s_pending_addr.toString().c_str());

        // Stop scanning — we'll reconnect after connection attempt
        NimBLEDevice::getScan()->stop();
        s_scanning = false;
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        s_scanning = false;
    }
};

static ScanCallbacks s_scan_callbacks;

// ─── Connection logic (runs in task_loop context) ───────────────────────────

/// Detect whether a HID device is a keyboard, mouse, or both.
/// Checks Boot Protocol characteristics first, then falls back to Report Map.
static void detect_device_type(NimBLERemoteService* hid_service,
                               bool& is_keyboard, bool& is_mouse) {
    is_keyboard = false;
    is_mouse = false;

    // Check for Boot Protocol characteristics (most reliable)
    if (hid_service->getCharacteristic(BOOT_KBD_INPUT_UUID)) {
        is_keyboard = true;
        Serial.println("[BLE] Detected keyboard (Boot Keyboard Input Report)");
    }
    if (hid_service->getCharacteristic(BOOT_MOUSE_INPUT_UUID)) {
        is_mouse = true;
        Serial.println("[BLE] Detected mouse (Boot Mouse Input Report)");
    }

    if (is_keyboard || is_mouse) return;

    // Fallback: parse Report Map for Usage Page / Usage hints
    NimBLERemoteCharacteristic* report_map = hid_service->getCharacteristic(REPORT_MAP_UUID);
    if (!report_map || !report_map->canRead()) return;

    std::string map_data = report_map->readValue();
    const uint8_t* d = (const uint8_t*)map_data.data();
    size_t len = map_data.length();

    Serial.printf("[BLE] Report Map (%d bytes), scanning for usage...\n", (int)len);

    // Simple HID descriptor scan: look for Usage Page (Generic Desktop) + Usage
    for (size_t i = 0; i + 1 < len; i++) {
        // Usage Page 0x01 = Generic Desktop
        if (d[i] == 0x05 && d[i + 1] == 0x01 && i + 3 < len) {
            // Next item should be Usage
            if (d[i + 2] == 0x09) {
                uint8_t usage = d[i + 3];
                if (usage == 0x06) {  // Keyboard
                    is_keyboard = true;
                    Serial.println("[BLE] Detected keyboard (Report Map Usage 0x06)");
                } else if (usage == 0x02) {  // Mouse
                    is_mouse = true;
                    Serial.println("[BLE] Detected mouse (Report Map Usage 0x02)");
                }
            }
        }
    }

    if (!is_keyboard && !is_mouse) {
        Serial.println("[BLE] Could not determine device type, defaulting to keyboard");
        is_keyboard = true;
    }
}

static bool try_connect(const NimBLEAddress& addr, const char* name) {
    bool need_kbd   = (s_keyboard.status.state == DeviceState::DISCONNECTED);
    bool need_mouse = (s_mouse.status.state == DeviceState::DISCONNECTED);
    if (!need_kbd && !need_mouse) return false;

    Serial.printf("[BLE] Clients before connect: %d\n",
                  NimBLEDevice::getCreatedClientCount());

    // Reuse existing client if available (NimBLE best practice)
    NimBLEClient* client = NimBLEDevice::getClientByPeerAddress(addr);
    if (!client) {
        client = NimBLEDevice::getDisconnectedClient();
    }
    if (!client) {
        client = NimBLEDevice::createClient();
    }
    // Connect first with neutral callbacks — avoids corrupting kbd/mouse state
    client->setClientCallbacks(&s_neutral_callbacks, false);
    client->setConnectionParams(12, 40, 0, 400);

    Serial.printf("[BLE] Connecting to %s...\n", name);

    if (!client->connect(addr)) {
        Serial.printf("[BLE] Connection failed to %s\n", name);
        NimBLEDevice::deleteClient(client);
        return false;
    }

    Serial.printf("[BLE] Connected! Clients now: %d. Discovering services...\n",
                  NimBLEDevice::getCreatedClientCount());

    if (!client->discoverAttributes()) {
        Serial.println("[BLE] Service discovery failed");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    }

    NimBLERemoteService* hid_service = client->getService(HID_SERVICE_UUID);
    if (!hid_service) {
        Serial.println("[BLE] HID service not found");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    }

    // Detect what kind of device this is
    bool dev_is_kbd = false, dev_is_mouse = false;
    detect_device_type(hid_service, dev_is_kbd, dev_is_mouse);

    // Assign to the correct slot based on detected type
    BleDevice* target = nullptr;
    ClientCallbacks* cb = nullptr;
    bool assign_as_kbd = false;

    if (dev_is_kbd && need_kbd) {
        target = &s_keyboard;
        cb = &s_kbd_callbacks;
        assign_as_kbd = true;
    } else if (dev_is_mouse && need_mouse) {
        target = &s_mouse;
        cb = &s_mouse_callbacks;
        assign_as_kbd = false;
    } else if (dev_is_kbd && !need_kbd && need_mouse) {
        Serial.println("[BLE] Already have a keyboard, skipping");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    } else if (dev_is_mouse && !need_mouse && need_kbd) {
        Serial.println("[BLE] Already have a mouse, skipping");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return false;
    } else {
        // Fallback: assign to whichever slot is free
        if (need_kbd) {
            target = &s_keyboard;
            cb = &s_kbd_callbacks;
            assign_as_kbd = true;
        } else {
            target = &s_mouse;
            cb = &s_mouse_callbacks;
            assign_as_kbd = false;
        }
    }

    // Now assign the correct callbacks for the chosen slot
    client->setClientCallbacks(cb, false);
    target->status.state = DeviceState::CONNECTING;
    strncpy(target->status.name, name, sizeof(target->status.name) - 1);
    target->client = client;

    // Ensure the connection is encrypted before subscribing.
    // HID devices require encryption for notifications to flow.
    if (!client->secureConnection()) {
        Serial.println("[BLE] WARNING: Failed to secure connection (pairing rejected?)");
    } else {
        Serial.println("[BLE] Connection secured");
    }

    // Set protocol mode: Boot Protocol for keyboards (simpler 8-byte reports),
    // Report Protocol for mice (trackpads often lack Boot Mouse support and
    // setting Boot Protocol silences all HID Report notifications).
    bool boot_protocol_set = false;
    static const NimBLEUUID PROTOCOL_MODE_UUID("2A4E");
    NimBLERemoteCharacteristic* proto_mode = hid_service->getCharacteristic(PROTOCOL_MODE_UUID);
    if (proto_mode && proto_mode->canWrite() && assign_as_kbd) {
        uint8_t mode = 0;  // 0=Boot
        if (proto_mode->writeValue(&mode, 1, false)) {
            boot_protocol_set = true;
            Serial.println("[BLE] Set Boot Protocol mode");
        } else {
            Serial.println("[BLE] Failed to set Boot Protocol mode");
        }
    }
    if (!boot_protocol_set && assign_as_kbd) {
        Serial.println("[BLE] Protocol Mode read-only — staying in Report Protocol");
    }

    // Subscribe to HID characteristics — selective strategy:
    // Keyboard: Boot KBD Input only (reliable 8-byte format).
    //           Skip HID Report chars — they include consumer/vendor reports
    //           that fire callbacks but get filtered, wasting NimBLE host task time.
    //           Fall back to HID Report only if no Boot KBD Input exists.
    // Mouse:    HID Report only (Report Protocol, since Boot Protocol write is
    //           a no-op on most devices). Skip Boot Mouse Input to avoid duplicate
    //           reports flooding the NimBLE host task.
    //           Fall back to Boot Mouse Input only if no HID Report exists.
    const auto& reports = hid_service->getCharacteristics(true);
    bool subscribed = false;

    auto cb_fn = assign_as_kbd ? on_keyboard_report : on_mouse_report;
    const char* type_str = assign_as_kbd ? "keyboard" : "mouse";

    if (assign_as_kbd) {
        // ── Keyboard subscription strategy ──
        // If Boot Protocol was set: use Boot KBD Input only (clean 8-byte reports).
        // If Report Protocol (read-only proto mode): use HID Report chars
        // (some are consumer/vendor noise, filtered by length check in callback).
        if (boot_protocol_set) {
            NimBLERemoteCharacteristic* boot_kbd = hid_service->getCharacteristic(BOOT_KBD_INPUT_UUID);
            if (boot_kbd && (boot_kbd->canNotify() || boot_kbd->canIndicate())) {
                bool use_indicate = !boot_kbd->canNotify();
                bool ok = boot_kbd->subscribe(!use_indicate, cb_fn);
                if (ok) {
                    subscribed = true;
                    Serial.printf("[BLE] Subscribed keyboard to Boot KBD Input (handle=%d)\n",
                                  boot_kbd->getHandle());
                }
            }
        }

        if (!subscribed) {
            // Report Protocol mode — subscribe to HID Report chars
            for (auto* chr : reports) {
                if (chr->getUUID() == HID_REPORT_UUID && chr->canNotify()) {
                    bool ok = chr->subscribe(true, cb_fn);
                    if (ok) {
                        subscribed = true;
                        Serial.printf("[BLE] Subscribed keyboard to HID Report (handle=%d)\n",
                                      chr->getHandle());
                    }
                }
            }
        }
    } else {
        // ── Mouse subscription strategy ──
        // Prefer HID Report (0x2A4D) — Report Protocol carries full deltas.
        // Subscribe to first notifiable HID Report only (avoid duplicates).
        bool got_hid_report = false;
        for (auto* chr : reports) {
            if (chr->getUUID() == HID_REPORT_UUID && chr->canNotify()) {
                bool ok = chr->subscribe(true, cb_fn);
                if (ok) {
                    subscribed = true;
                    got_hid_report = true;
                    Serial.printf("[BLE] Subscribed mouse to HID Report (handle=%d)\n",
                                  chr->getHandle());
                    break;  // one HID Report is enough
                }
            }
        }

        if (!got_hid_report) {
            // Fallback: use Boot Mouse Input
            NimBLERemoteCharacteristic* boot_mouse = hid_service->getCharacteristic(BOOT_MOUSE_INPUT_UUID);
            if (boot_mouse && (boot_mouse->canNotify() || boot_mouse->canIndicate())) {
                bool use_indicate = !boot_mouse->canNotify();
                bool ok = boot_mouse->subscribe(!use_indicate, cb_fn);
                if (ok) {
                    subscribed = true;
                    Serial.printf("[BLE] Subscribed mouse to Boot Mouse Input (handle=%d)\n",
                                  boot_mouse->getHandle());
                }
            }
        }
    }

    // Log what we skipped
    int skipped = 0;
    for (auto* chr : reports) {
        if ((chr->canNotify() || chr->canIndicate()) &&
            (chr->getUUID() == HID_REPORT_UUID ||
             chr->getUUID() == BOOT_KBD_INPUT_UUID ||
             chr->getUUID() == BOOT_MOUSE_INPUT_UUID)) {
            skipped++;
        }
    }
    Serial.printf("[BLE] %s: subscribed to %d, skipped %d notifiable HID chars\n",
                  type_str, subscribed ? 1 : 0, skipped - (subscribed ? 1 : 0));

    if (subscribed) {
        // Verify connection is still alive after subscription
        if (!client->isConnected()) {
            Serial.println("[BLE] WARNING: Connection lost during subscription!");
            target->status.state = DeviceState::DISCONNECTED;
            target->client = nullptr;
            NimBLEDevice::deleteClient(client);
            return false;
        }

        target->status.state = DeviceState::CONNECTED;
        target->status.is_keyboard = assign_as_kbd;
        target->status.is_mouse = !assign_as_kbd;
        target->bonded_addr = client->getPeerAddress();
        target->reconnect_attempts = 0;
        Serial.printf("[BLE] %s ready: %s (conn handle=%d)\n",
                      target->status.is_keyboard ? "Keyboard" : "Mouse",
                      name, client->getConnHandle());
        return true;
    }

    Serial.println("[BLE] No subscribable HID reports found");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    target->status.state = DeviceState::DISCONNECTED;
    target->client = nullptr;
    return false;
}

// ─── Report parsing ─────────────────────────────────────────────────────────

// Per-handle tracking: which characteristic handles are firing and how often
static constexpr int MAX_TRACKED_HANDLES = 10;
struct HandleStats {
    uint16_t handle;
    uint32_t count;
};
static HandleStats s_kbd_handle_stats[MAX_TRACKED_HANDLES] = {};
static HandleStats s_mouse_handle_stats[MAX_TRACKED_HANDLES] = {};

static void track_handle(HandleStats* stats, uint16_t handle) {
    for (int i = 0; i < MAX_TRACKED_HANDLES; i++) {
        if (stats[i].handle == handle) { stats[i].count++; return; }
        if (stats[i].count == 0) { stats[i].handle = handle; stats[i].count = 1; return; }
    }
}

static volatile uint32_t s_ble_kbd_cb_count = 0;
static volatile uint32_t s_ble_kbd_cb_used = 0;    // reports that passed length filter
static volatile uint32_t s_ble_kbd_cb_dropped = 0;  // reports rejected by length filter
static volatile uint32_t s_ble_kbd_last_ms = 0;     // millis() of last keyboard notification

static void on_keyboard_report(NimBLERemoteCharacteristic* chr,
                               uint8_t* data, size_t length, bool is_notify) {
    s_ble_kbd_cb_count++;
    s_ble_kbd_last_ms = millis();
    track_handle(s_kbd_handle_stats, chr->getHandle());

    if (length < 8) {
        s_ble_kbd_cb_dropped++;
        return;
    }
    s_ble_kbd_cb_used++;

    uint8_t modifiers = data[0];

    // Process modifier key changes
    uint8_t mod_diff = modifiers ^ s_keyboard.prev_modifiers;
    if (mod_diff) {
        for (int i = 0; i < keycode_map::MODIFIER_MAP_SIZE; i++) {
            uint8_t mask = keycode_map::MODIFIER_MAP[i].usb_mask;
            if (mod_diff & mask) {
                KbdEvent evt;
                evt.adb_keycode = keycode_map::MODIFIER_MAP[i].adb_keycode;
                evt.released = !(modifiers & mask);
                event_queue::send_kbd(evt);

#if ADB_DEBUG_VERBOSE
                Serial.printf("[BLE] Modifier %s: ADB=0x%02X\n",
                              evt.released ? "up" : "down", evt.adb_keycode);
#endif
            }
        }
        s_keyboard.prev_modifiers = modifiers;
    }

    // Detect releases: keys in prev but not in current
    for (int i = 0; i < 6; i++) {
        uint8_t prev_key = s_keyboard.prev_keys[i];
        if (prev_key == 0) continue;

        bool still_pressed = false;
        for (int j = 2; j < 8 && j < (int)length; j++) {
            if (data[j] == prev_key) {
                still_pressed = true;
                break;
            }
        }

        if (!still_pressed) {
            uint8_t adb_code = keycode_map::usb_to_adb(prev_key);
            if (adb_code != keycode_map::ADB_KEY_NONE) {
                KbdEvent evt;
                evt.adb_keycode = adb_code;
                evt.released = true;
                event_queue::send_kbd(evt);

#if ADB_DEBUG_VERBOSE
                Serial.printf("[BLE] Key up: USB=0x%02X ADB=0x%02X\n", prev_key, adb_code);
#endif
            }
        }
    }

    // Detect presses: keys in current but not in prev
    for (int j = 2; j < 8 && j < (int)length; j++) {
        uint8_t cur_key = data[j];
        if (cur_key == 0) continue;

        bool was_pressed = false;
        for (int i = 0; i < 6; i++) {
            if (s_keyboard.prev_keys[i] == cur_key) {
                was_pressed = true;
                break;
            }
        }

        if (!was_pressed) {
            uint8_t adb_code = keycode_map::usb_to_adb(cur_key);
            if (adb_code != keycode_map::ADB_KEY_NONE) {
                KbdEvent evt;
                evt.adb_keycode = adb_code;
                evt.released = false;
                event_queue::send_kbd(evt);

#if ADB_DEBUG_VERBOSE
                Serial.printf("[BLE] Key down: USB=0x%02X ADB=0x%02X\n", cur_key, adb_code);
#endif
            }
        }
    }

    // Save current state for next diff
    for (int i = 0; i < 6; i++) {
        s_keyboard.prev_keys[i] = (i + 2 < (int)length) ? data[i + 2] : 0;
    }
}

static volatile uint32_t s_ble_mouse_cb_count = 0;
static volatile uint32_t s_ble_mouse_last_ms = 0;   // millis() of last mouse notification

static void on_mouse_report(NimBLERemoteCharacteristic* chr,
                            uint8_t* data, size_t length, bool is_notify) {
    s_ble_mouse_cb_count++;
    s_ble_mouse_last_ms = millis();
    track_handle(s_mouse_handle_stats, chr->getHandle());
    if (length < 3) return;

    MouseEvent evt;

    if (length >= 5) {
        // Report Protocol: 5-7 bytes
        // [buttons] [X_lo] [X_hi] [Y_lo] [Y_hi] [scroll_lo] [scroll_hi]
        evt.button = (data[0] & 0x01) != 0;
        evt.dx = (int16_t)(data[1] | (data[2] << 8));
        evt.dy = (int16_t)(data[3] | (data[4] << 8));
    } else {
        // Boot Protocol: 3 bytes [buttons] [dx_8bit] [dy_8bit]
        evt.button = (data[0] & 0x01) != 0;
        evt.dx = (int8_t)data[1];
        evt.dy = (int8_t)data[2];
    }

    event_queue::send_mouse(evt);

#if ADB_DEBUG_VERBOSE
    if (evt.dx != 0 || evt.dy != 0 || evt.button != s_mouse.prev_buttons) {
        Serial.printf("[BLE] Mouse: btn=%d dx=%d dy=%d\n",
                      evt.button, evt.dx, evt.dy);
    }
#endif

    s_mouse.prev_buttons = evt.button;
}

// ─── Reconnection ────────────────────────────────────────────────────────────

/// Attempt to reconnect to a previously-bonded device using stored address.
/// Reuses the existing client object (preserves bond keys for fast encryption).
static bool try_reconnect(BleDevice* device, const char* label) {
    NimBLEClient* client = device->client;

    // If client was cleaned up, get one by peer address or a disconnected one
    if (!client) {
        client = NimBLEDevice::getClientByPeerAddress(device->bonded_addr);
        if (!client) {
            client = NimBLEDevice::getDisconnectedClient();
        }
        if (!client) {
            client = NimBLEDevice::createClient();
        }
        device->client = client;
    }

    // Set the correct callbacks before connecting
    bool is_kbd = device->was_keyboard;
    ClientCallbacks* cb = is_kbd ? &s_kbd_callbacks : &s_mouse_callbacks;
    client->setClientCallbacks(cb, false);
    client->setConnectionParams(12, 40, 0, 400);

    Serial.printf("[BLE] [%s] Reconnecting to %s (attempt %d)...\n",
                  label, device->bonded_addr.toString().c_str(),
                  device->reconnect_attempts + 1);

    if (!client->connect(device->bonded_addr, BLE_RECONNECT_TIMEOUT_MS)) {
        Serial.printf("[BLE] [%s] Reconnect failed\n", label);
        return false;
    }

    Serial.printf("[BLE] [%s] Reconnected! Securing...\n", label);

    // Re-encrypt using stored bond keys (fast — no user interaction)
    if (!client->secureConnection()) {
        Serial.printf("[BLE] [%s] WARNING: Failed to secure reconnection\n", label);
    }

    // Rediscover services
    if (!client->discoverAttributes()) {
        Serial.printf("[BLE] [%s] Service rediscovery failed\n", label);
        client->disconnect();
        return false;
    }

    NimBLERemoteService* hid_service = client->getService(HID_SERVICE_UUID);
    if (!hid_service) {
        Serial.printf("[BLE] [%s] HID service not found on reconnect\n", label);
        client->disconnect();
        return false;
    }

    // Resubscribe to HID characteristics (same strategy as initial connect)
    const auto& reports = hid_service->getCharacteristics(true);
    auto cb_fn = is_kbd ? on_keyboard_report : on_mouse_report;
    bool subscribed = false;

    if (is_kbd) {
        // Keyboard: prefer Boot KBD Input, fall back to HID Report
        NimBLERemoteCharacteristic* boot_kbd = hid_service->getCharacteristic(BOOT_KBD_INPUT_UUID);
        if (boot_kbd && boot_kbd->canNotify()) {
            if (boot_kbd->subscribe(true, cb_fn)) {
                subscribed = true;
                Serial.printf("[BLE] [%s] Resubscribed to Boot KBD Input\n", label);
            }
        }
        if (!subscribed) {
            for (auto* chr : reports) {
                if (chr->getUUID() == HID_REPORT_UUID && chr->canNotify()) {
                    if (chr->subscribe(true, cb_fn)) {
                        subscribed = true;
                        Serial.printf("[BLE] [%s] Resubscribed to HID Report (handle=%d)\n",
                                      label, chr->getHandle());
                    }
                }
            }
        }
    } else {
        // Mouse: prefer HID Report, fall back to Boot Mouse Input
        for (auto* chr : reports) {
            if (chr->getUUID() == HID_REPORT_UUID && chr->canNotify()) {
                if (chr->subscribe(true, cb_fn)) {
                    subscribed = true;
                    Serial.printf("[BLE] [%s] Resubscribed to HID Report (handle=%d)\n",
                                  label, chr->getHandle());
                    break;
                }
            }
        }
        if (!subscribed) {
            NimBLERemoteCharacteristic* boot_mouse = hid_service->getCharacteristic(BOOT_MOUSE_INPUT_UUID);
            if (boot_mouse && (boot_mouse->canNotify() || boot_mouse->canIndicate())) {
                bool use_indicate = !boot_mouse->canNotify();
                if (boot_mouse->subscribe(!use_indicate, cb_fn)) {
                    subscribed = true;
                    Serial.printf("[BLE] [%s] Resubscribed to Boot Mouse Input\n", label);
                }
            }
        }
    }

    if (!subscribed || !client->isConnected()) {
        Serial.printf("[BLE] [%s] Reconnect subscription failed\n", label);
        client->disconnect();
        return false;
    }

    // Restore connected state — device type already known
    device->status.state = DeviceState::CONNECTED;
    device->status.is_keyboard = device->was_keyboard;
    device->status.is_mouse = device->was_mouse;
    device->reconnect_attempts = 0;
    device->bonded_addr = client->getPeerAddress();
    Serial.printf("[BLE] [%s] Reconnected and ready\n", label);
    return true;
}

/// Manage reconnection backoff for a single device.
static void handle_reconnection(BleDevice* device, const char* label) {
    if (device->status.state != DeviceState::RECONNECTING) return;

    uint32_t now = millis();
    if ((int32_t)(now - device->reconnect_next_ms) < 0) return;  // not yet time

    if (try_reconnect(device, label)) {
        return;  // success
    }

    device->reconnect_attempts++;
    if (device->reconnect_attempts >= BLE_RECONNECT_MAX_ATTEMPTS) {
        Serial.printf("[BLE] [%s] Giving up reconnection after %d attempts\n",
                      label, device->reconnect_attempts);
        device->status.state = DeviceState::DISCONNECTED;
        device->status.is_keyboard = false;
        device->status.is_mouse = false;
        // Clean up client to free the slot
        if (device->client) {
            NimBLEDevice::deleteClient(device->client);
            device->client = nullptr;
        }
        return;
    }

    // Exponential backoff: double the delay, capped at max
    device->reconnect_delay_ms = std::min(device->reconnect_delay_ms * 2,
                                          BLE_RECONNECT_MAX_MS);
    device->reconnect_next_ms = now + device->reconnect_delay_ms;
    Serial.printf("[BLE] [%s] Next reconnect in %lums (attempt %d/%d)\n",
                  label, device->reconnect_delay_ms,
                  device->reconnect_attempts, BLE_RECONNECT_MAX_ATTEMPTS);
}

// ─── Scanning ───────────────────────────────────────────────────────────────

static void start_scan() {
    if (s_scanning) return;

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&s_scan_callbacks, false);  // false = don't delete
    scan->setInterval(BLE_SCAN_INTERVAL_MS);
    scan->setWindow(BLE_SCAN_WINDOW_MS);
    scan->setActiveScan(true);
    scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL_INITA);  // detect directed ads from bonded RPAs
    scan->start(BLE_SCAN_DURATION_S, false);
    s_scanning = true;

    Serial.println("[BLE] Scanning for HID devices...");
}

// ─── Public interface ───────────────────────────────────────────────────────

void init() {
    NimBLEDevice::init("ADB-Bridge");
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    Serial.println("[BLE] NimBLE initialized");
    start_scan();
}

void task_loop() {
    Serial.println("[BLE] Task loop started on core " + String(xPortGetCoreID()));

    while (true) {
        // Handle pending connection (deferred from scan callback)
        if (s_pending_connect) {
            s_pending_connect = false;
            try_connect(s_pending_addr, s_pending_name);

            // Resume scanning if we still need devices
            if (s_keyboard.status.state == DeviceState::DISCONNECTED ||
                s_mouse.status.state == DeviceState::DISCONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(2000));  // pause before re-scan
                start_scan();
            }
        }

        // Connection health check: detect silent disconnects → enter RECONNECTING
        if (s_keyboard.status.state == DeviceState::CONNECTED && s_keyboard.client) {
            if (!s_keyboard.client->isConnected()) {
                Serial.println("[BLE] [KBD] Silent disconnect detected");
                // Trigger the same path as onDisconnect: enter RECONNECTING
                s_keyboard.was_keyboard = true;
                s_keyboard.was_mouse = false;
                s_keyboard.bonded_addr = s_keyboard.client->getPeerAddress();
                s_keyboard.reconnect_delay_ms = BLE_RECONNECT_INITIAL_MS;
                s_keyboard.reconnect_next_ms = millis() + BLE_RECONNECT_INITIAL_MS;
                s_keyboard.reconnect_attempts = 0;
                s_keyboard.status.state = DeviceState::RECONNECTING;
            }
        }
        if (s_mouse.status.state == DeviceState::CONNECTED && s_mouse.client) {
            if (!s_mouse.client->isConnected()) {
                Serial.println("[BLE] [MOU] Silent disconnect detected");
                s_mouse.was_keyboard = false;
                s_mouse.was_mouse = true;
                s_mouse.bonded_addr = s_mouse.client->getPeerAddress();
                s_mouse.reconnect_delay_ms = BLE_RECONNECT_INITIAL_MS;
                s_mouse.reconnect_next_ms = millis() + BLE_RECONNECT_INITIAL_MS;
                s_mouse.reconnect_attempts = 0;
                s_mouse.status.state = DeviceState::RECONNECTING;
            }
        }

        // Handle reconnection attempts with exponential backoff
        handle_reconnection(&s_keyboard, "KBD");
        handle_reconnection(&s_mouse, "MOU");

        // Check for DISCONNECTED devices (not RECONNECTING) and restart scanning
        if (!s_scanning && !s_pending_connect) {
            if (s_keyboard.status.state == DeviceState::DISCONNECTED ||
                s_mouse.status.state == DeviceState::DISCONNECTED) {
                start_scan();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

DeviceStatus get_keyboard_status() {
    return s_keyboard.status;
}

DeviceStatus get_mouse_status() {
    return s_mouse.status;
}

bool keyboard_connected() {
    return s_keyboard.status.state == DeviceState::CONNECTED;
}

bool mouse_connected() {
    return s_mouse.status.state == DeviceState::CONNECTED;
}

uint32_t get_mouse_cb_count() {
    return s_ble_mouse_cb_count;
}

uint32_t get_kbd_cb_count() {
    return s_ble_kbd_cb_count;
}

uint32_t get_kbd_cb_used() {
    return s_ble_kbd_cb_used;
}

uint32_t get_kbd_cb_dropped() {
    return s_ble_kbd_cb_dropped;
}

uint32_t get_kbd_last_ms() {
    return s_ble_kbd_last_ms;
}

uint32_t get_mouse_last_ms() {
    return s_ble_mouse_last_ms;
}

void dump_handle_stats() {
    Serial.print("[DIAG] KBD handles: ");
    for (int i = 0; i < MAX_TRACKED_HANDLES && s_kbd_handle_stats[i].count; i++) {
        Serial.printf("h%d=%lu ", s_kbd_handle_stats[i].handle, s_kbd_handle_stats[i].count);
    }
    Serial.println();
    Serial.print("[DIAG] MOU handles: ");
    for (int i = 0; i < MAX_TRACKED_HANDLES && s_mouse_handle_stats[i].count; i++) {
        Serial.printf("h%d=%lu ", s_mouse_handle_stats[i].handle, s_mouse_handle_stats[i].count);
    }
    Serial.println();
}

} // namespace ble_hid_host
