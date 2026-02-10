#include "event_queue.h"
#include "config.h"

namespace event_queue {

static QueueHandle_t s_kbd_queue   = nullptr;
static QueueHandle_t s_mouse_queue = nullptr;

void init() {
    s_kbd_queue   = xQueueCreate(KBD_QUEUE_SIZE,   sizeof(KbdEvent));
    s_mouse_queue = xQueueCreate(MOUSE_QUEUE_SIZE, sizeof(MouseEvent));
}

QueueHandle_t kbd_queue() {
    return s_kbd_queue;
}

QueueHandle_t mouse_queue() {
    return s_mouse_queue;
}

bool send_kbd(const KbdEvent& evt) {
    return xQueueSend(s_kbd_queue, &evt, 0) == pdTRUE;
}

bool send_mouse(const MouseEvent& evt) {
    return xQueueSend(s_mouse_queue, &evt, 0) == pdTRUE;
}

bool receive_kbd(KbdEvent& evt) {
    return xQueueReceive(s_kbd_queue, &evt, 0) == pdTRUE;
}

bool receive_mouse(MouseEvent& evt) {
    return xQueueReceive(s_mouse_queue, &evt, 0) == pdTRUE;
}

bool kbd_pending() {
    return uxQueueMessagesWaiting(s_kbd_queue) > 0;
}

bool mouse_pending() {
    return uxQueueMessagesWaiting(s_mouse_queue) > 0;
}

} // namespace event_queue
