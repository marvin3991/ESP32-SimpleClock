#pragma once
#include <Arduino.h>

// Serial log. Only call from the Arduino loop task: callbacks running in
// other tasks (SNTP, Wi-Fi events) must just set flags.
// g_log_mute is raised while a screenshot streams binary data on Serial.
extern volatile bool g_log_mute;

#define LOGI(tag, fmt, ...)                                                     \
    do {                                                                        \
        if (!g_log_mute)                                                        \
            Serial.printf("[%8lu] " tag ": " fmt "\n", (unsigned long)millis(), \
                          ##__VA_ARGS__);                                       \
    } while (0)

#define LOGE(tag, fmt, ...) LOGI(tag, "ERROR " fmt, ##__VA_ARGS__)
