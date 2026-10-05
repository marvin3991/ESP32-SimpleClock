#pragma once
#include <stdint.h>

enum SetupPhase : uint8_t { SETUP_OFF, SETUP_WAIT, SETUP_CONNECTING, SETUP_OK, SETUP_FAIL };

void net_init();               // STA if credentials exist, otherwise Wi-Fi setup
void net_loop();
void net_start_setup();        // SoftAP + captive portal
void net_stop_setup();         // back to station mode (needs credentials)
bool net_in_setup();
SetupPhase net_setup_phase();
const char* net_setup_message();   // short ASCII text for the panel
bool net_connected();
const char* net_ip();
int net_rssi();
const char* net_ap_ssid();
const char* net_ap_pass();
const char* net_ap_ip();
const char* net_last_error();      // ASCII, "" if none
uint32_t net_disconnects();
