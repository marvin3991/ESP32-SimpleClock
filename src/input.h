#pragma once
#include <stdint.h>

enum Button : uint8_t { BTN_BOOT, BTN_KEY, BTN_PWR, BTN_TOUCH, BTN_COUNT };
enum InputKind : uint8_t { IN_PRESS, IN_SHORT, IN_LONG };

struct InputEvent {
    Button button;
    InputKind kind;
};

void input_init();
void input_tick();                       // poll buttons, PMU key, touch
bool input_next(InputEvent* ev);         // pop one queued event
void input_inject(Button b, InputKind k); // console / testing
const char* input_button_name(Button b);
