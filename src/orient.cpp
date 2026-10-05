#include "orient.h"

#include <Arduino.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>
#include <math.h>

#include "board.h"
#include "config.h"
#include "log.h"

static const uint8_t UNKNOWN = 255;   // lying flat: keep the last orientation

static SensorQMI8658 s_imu;
static bool s_ok = false;
static uint8_t s_current = 0, s_candidate = 0;
static uint32_t s_candidate_since = 0, s_last_poll = 0;

// Raw accelerometer quadrant from the dominant in-plane axis.
static uint8_t accel_to_raw(float ax, float ay) {
    const float abs_x = fabsf(ax), abs_y = fabsf(ay);
    if (abs_x < IMU_TILT_G && abs_y < IMU_TILT_G) return UNKNOWN;
    if (abs_y > abs_x) return ay > 0 ? 3 : 1;
    return ax > 0 ? 0 : 2;
}

// The IMU sits on the PCB rotated 90 deg against the panel raster, so the raw
// quadrant is one step behind (checked on the real board).
static uint8_t raw_to_quadrant(uint8_t raw) { return (uint8_t)((raw + 1) & 3); }

bool orient_read(float* ax, float* ay, float* az) {
    if (!s_ok) return false;
    return s_imu.getAccelerometer(*ax, *ay, *az);
}

bool orient_init() {
    if (!s_imu.begin(Wire, QMI8658_ADDR, I2C_SDA, I2C_SCL)) {
        LOGE("imu", "QMI8658 not found - auto rotation disabled");
        return false;
    }
    s_imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
                              SensorQMI8658::LPF_MODE_3);
    s_imu.enableAccelerometer();
    s_ok = true;
    // Take the boot orientation immediately so the first frame is upright.
    for (int i = 0; i < 10; ++i) {
        delay(60);   // ODR 21 Hz -> a fresh sample every ~48 ms
        float ax, ay, az;
        if (!orient_read(&ax, &ay, &az)) continue;
        const uint8_t raw = accel_to_raw(ax, ay);
        if (raw != UNKNOWN) {
            s_current = s_candidate = raw_to_quadrant(raw);
            break;
        }
    }
    LOGI("imu", "ready, quadrant %u", s_current);
    return true;
}

void orient_tick() {
    if (!s_ok) return;
    const uint32_t now = millis();
    if (now - s_last_poll < IMU_POLL_MS) return;
    s_last_poll = now;
    float ax, ay, az;
    if (!orient_read(&ax, &ay, &az)) return;
    const uint8_t raw = accel_to_raw(ax, ay);
    if (raw == UNKNOWN) {
        s_candidate = s_current;
        return;
    }
    const uint8_t q = raw_to_quadrant(raw);
    if (q == s_current) {
        s_candidate = q;
        return;
    }
    if (q != s_candidate) {
        s_candidate = q;
        s_candidate_since = now;
    } else if (now - s_candidate_since >= IMU_STABLE_MS) {
        s_current = q;
        LOGI("imu", "orientation -> quadrant %u", q);
    }
}

bool orient_ok() { return s_ok; }
uint8_t orient_quadrant() { return s_current; }
