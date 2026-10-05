#include "input.h"

#include <Arduino.h>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>

#include "board.h"
#include "config.h"
#include "log.h"
#include "power.h"

// If the touch controller sends no report for this long, the finger is
// considered lifted even if the "release" report was missed.
static const uint32_t TOUCH_RELEASE_MS = 200;

struct GpioButton {
    uint8_t pin;
    Button id;
    uint16_t long_ms;
    bool raw, stable, long_sent;
    uint32_t raw_since, pressed_at;
};

static GpioButton s_buttons[] = {
    {BTN_BOOT_GPIO, BTN_BOOT, BTN_BOOT_LONG_MS, false, false, false, 0, 0},
    {BTN_KEY_GPIO, BTN_KEY, BTN_KEY_LONG_MS, false, false, false, 0, 0},
};

static InputEvent s_queue[16];
static uint8_t s_head = 0, s_tail = 0;

static TouchDrvCST92xx s_touch;
static bool s_touch_ok = false;
static volatile bool s_touch_irq = false;
static bool s_touch_down = false;
static uint32_t s_touch_last_report = 0, s_last_tap = 0;

static void IRAM_ATTR on_touch_irq() { s_touch_irq = true; }

static void push(Button b, InputKind k) {
    const uint8_t next = (uint8_t)((s_head + 1) % (sizeof(s_queue) / sizeof(s_queue[0])));
    if (next == s_tail) return;   // queue full: drop (cannot happen at human speeds)
    s_queue[s_head] = InputEvent{b, k};
    s_head = next;
}

void input_init() {
    for (auto& b : s_buttons) {
        pinMode(b.pin, INPUT_PULLUP);
        b.raw = b.stable = (digitalRead(b.pin) == LOW);
        // A button held through boot (e.g. BOOT after flashing) must not fire.
        b.long_sent = b.stable;
    }
    s_touch.setPins(TP_RST, TP_INT);
    if (s_touch.begin(Wire, CST9220_ADDR, I2C_SDA, I2C_SCL)) {
        s_touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        pinMode(TP_INT, INPUT_PULLUP);
        attachInterrupt(TP_INT, on_touch_irq, FALLING);
        s_touch_ok = true;
        LOGI("input", "touch ready (%s)", s_touch.getModelName());
    } else {
        LOGE("input", "touch controller not found - taps disabled");
    }
}

static void poll_gpio(uint32_t now) {
    for (auto& b : s_buttons) {
        const bool raw = digitalRead(b.pin) == LOW;
        if (raw != b.raw) {
            b.raw = raw;
            b.raw_since = now;
        }
        if (raw != b.stable && now - b.raw_since >= BTN_DEBOUNCE_MS) {
            b.stable = raw;
            if (raw) {
                b.pressed_at = now;
                b.long_sent = false;
                push(b.id, IN_PRESS);
            } else if (!b.long_sent) {
                push(b.id, IN_SHORT);
            }
        }
        if (b.stable && !b.long_sent && now - b.pressed_at >= b.long_ms) {
            b.long_sent = true;
            push(b.id, IN_LONG);
        }
    }
}

static void poll_touch(uint32_t now) {
    if (!s_touch_ok) return;
    if (s_touch_irq) {
        s_touch_irq = false;
        int16_t x[5], y[5];
        const uint8_t n = s_touch.getPoint(x, y, 1);
        s_touch_last_report = now;
        if (n > 0 && !s_touch_down) {
            s_touch_down = true;
            if (now - s_last_tap >= TAP_GUARD_MS) {
                s_last_tap = now;
                push(BTN_TOUCH, IN_SHORT);
            }
        } else if (n == 0) {
            s_touch_down = false;
        }
    } else if (s_touch_down && now - s_touch_last_report > TOUCH_RELEASE_MS) {
        s_touch_down = false;
    }
}

void input_tick() {
    const uint32_t now = millis();
    poll_gpio(now);
    if (power_pwr_pressed()) push(BTN_PWR, IN_SHORT);
    poll_touch(now);
}

bool input_next(InputEvent* ev) {
    if (s_tail == s_head) return false;
    *ev = s_queue[s_tail];
    s_tail = (uint8_t)((s_tail + 1) % (sizeof(s_queue) / sizeof(s_queue[0])));
    return true;
}

void input_inject(Button b, InputKind k) { push(b, k); }

const char* input_button_name(Button b) {
    switch (b) {
        case BTN_BOOT: return "BOOT";
        case BTN_KEY: return "KEY";
        case BTN_PWR: return "PWR";
        case BTN_TOUCH: return "TOUCH";
        default: return "?";
    }
}
