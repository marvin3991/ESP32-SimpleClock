// The parts of Arduino.h that the firmware sources under test use, for host
// builds (tests/run.sh). The simulation defines millis(), delay() and Serial.
#pragma once
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>

uint32_t millis();
void delay(uint32_t ms);
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
template <class A, class B>
static inline auto min(A a, B b) -> decltype(a < b ? a : b) {
    return a < b ? a : b;
}

struct FakeSerial {
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
extern FakeSerial Serial;
