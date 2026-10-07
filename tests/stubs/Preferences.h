// The ESP32 Preferences (NVS) API for host builds: one in-memory store that
// the simulation can preset and inspect, keyed "namespace/key".
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>

extern std::map<std::string, int> g_nvs;

class Preferences {
  public:
    bool begin(const char* ns, bool read_only = false) {
        (void)read_only;
        ns_ = ns;
        return true;
    }
    void end() {}
    bool isKey(const char* key) { return g_nvs.count(ns_ + "/" + key) > 0; }
    int8_t getChar(const char* key, int8_t def = 0) {
        const auto it = g_nvs.find(ns_ + "/" + key);
        return it == g_nvs.end() ? def : (int8_t)it->second;
    }
    size_t putChar(const char* key, int8_t v) {
        g_nvs[ns_ + "/" + key] = v;
        return 1;
    }

  private:
    std::string ns_;
};
