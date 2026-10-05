#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi_types.h>

#include "config.h"
#include "log.h"
#include "power.h"
#include "settings.h"
#include "timekeep.h"
#include "web_page.h"

// No answer (GOT_IP or DISCONNECTED) this long after WiFi.begin() counts as
// a failed attempt.
static const uint32_t ATTEMPT_TIMEOUT_MS = 30000;
// Leave setup mode by itself after this long without any web request, but
// only if working credentials already exist (otherwise there is nothing to
// go back to).
static const uint32_t SETUP_IDLE_EXIT_MS = 10UL * 60UL * 1000UL;
static const uint32_t SCAN_MAX_AGE_MS = 20000;
static const uint32_t POWER_CHECK_MS = 5000;

enum Mode : uint8_t { MODE_STA, MODE_SETUP };

static Mode s_mode = MODE_STA;
static bool s_connected = false;
static bool s_attempting = false;
static uint32_t s_attempt_at = 0, s_retry_at = 0, s_backoff = WIFI_RETRY_MIN_MS;
static uint32_t s_disconnects = 0;
static char s_ip[16] = "";
static char s_err[40] = "";
static bool s_mdns = false;
// Set when we drop the link ourselves; the resulting ASSOC_LEAVE event is not
// a failure of the next attempt.
static bool s_leaving = false;

static volatile bool s_ev_got_ip = false, s_ev_disc = false;
static volatile uint8_t s_ev_reason = 0;

static SetupPhase s_phase = SETUP_OFF;
static uint32_t s_phase_at = 0, s_last_web = 0;
static char s_ap_ssid[24] = "", s_ap_pass[12] = "", s_ap_ip[16] = "192.168.4.1";
static bool s_reconnect_pending = false;
static uint32_t s_scan_at = 0;
static int8_t s_sleep_mode = -1;   // last WiFi.setSleep() value, -1 = not set yet
static uint32_t s_power_check_at = 0;

static DNSServer s_dns;
static WebServer s_web(80);

// ------------------------------------------------------------------ helpers

static const char* reason_text(uint8_t r) {
    switch (r) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "WRONG PASSWORD";
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "NETWORK NOT FOUND";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "SIGNAL LOST";
        default:
            return "CONNECT FAILED";
    }
}

static void json_str(String& out, const char* s) {
    out += '"';
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += (char)c;
        }
    }
    out += '"';
}

static void begin_attempt() {
    if (!settings_has_wifi()) return;
    WiFi.begin(g_settings.ssid, g_settings.pass);
    s_attempting = true;
    s_attempt_at = millis();
    LOGI("net", "connecting to \"%s\"", g_settings.ssid);
}

static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    // Runs in the Wi-Fi event task: record only, handled in net_loop().
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        s_ev_got_ip = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        s_ev_reason = info.wifi_sta_disconnected.reason;
        s_ev_disc = true;
    }
}

// ------------------------------------------------------------------ web API

static void send_json(int code, const String& body) {
    s_web.sendHeader("Cache-Control", "no-store");
    s_web.send(code, "application/json", body);
}

static void handle_root() {
    s_last_web = millis();
    s_web.sendHeader("Cache-Control", "no-store");
    s_web.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
}

static const char* phase_name(SetupPhase p) {
    switch (p) {
        case SETUP_WAIT: return "wait";
        case SETUP_CONNECTING: return "connecting";
        case SETUP_OK: return "ok";
        case SETUP_FAIL: return "fail";
        default: return "off";
    }
}

static void handle_state() {
    s_last_web = millis();
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[32];
    String j;
    j.reserve(900);
    j += "{\"fw\":";
    json_str(j, FW_VERSION);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    j += ",\"time\":";
    json_str(j, buf);
    j += ",\"valid\":";
    j += timekeep_valid() ? "true" : "false";
    j += ",\"source\":";
    json_str(j, timekeep_source_name());
    const time_t ls = timekeep_last_sync();
    buf[0] = 0;
    if (ls) {
        localtime_r(&ls, &lt);
        strftime(buf, sizeof(buf), "%m-%d %H:%M", &lt);
    }
    j += ",\"last_sync\":";
    json_str(j, buf);
    j += ",\"syncs\":" + String(timekeep_sync_count());
    j += ",\"wifi\":{\"ssid\":";
    json_str(j, g_settings.ssid);
    j += ",\"connected\":";
    j += s_connected ? "true" : "false";
    j += ",\"ip\":";
    json_str(j, s_ip);
    j += ",\"rssi\":" + String(s_connected ? WiFi.RSSI() : 0);
    j += ",\"err\":";
    json_str(j, s_err);
    j += "},\"setup\":{\"phase\":";
    json_str(j, phase_name(s_phase));
    j += ",\"reason\":";   // ASCII code; the page translates it
    json_str(j, s_err);
    j += "},\"cfg\":{\"ssid\":";
    json_str(j, g_settings.ssid);
    j += ",\"has_pass\":";
    j += g_settings.pass[0] ? "true" : "false";
    j += ",\"tz\":";
    json_str(j, g_settings.tz);
    j += ",\"h12\":";
    j += g_settings.h12 ? "true" : "false";
    j += ",\"date\":";
    j += g_settings.show_date ? "true" : "false";
    j += ",\"night\":";
    j += g_settings.night_enabled ? "true" : "false";
    j += ",\"ns\":" + String(g_settings.night_start);
    j += ",\"ne\":" + String(g_settings.night_end);
    j += ",\"lday\":" + String(g_settings.level_day);
    j += ",\"lnight\":" + String(g_settings.level_night);
    j += ",\"rot\":" + String(g_settings.rotation);
    j += ",\"sleep\":";
    j += g_settings.sleep_enabled ? "true" : "false";
    j += ",\"ss\":" + String(g_settings.sleep_start);
    j += ",\"se\":" + String(g_settings.sleep_end);
    j += ",\"swap\":";
    j += g_settings.swap_hourly ? "true" : "false";
    j += ",\"ntp\":[";
    for (int i = 0; i < NTP_SERVERS; ++i) {
        if (i) j += ',';
        json_str(j, g_settings.ntp[i]);
    }
    j += ']';
    j += "},\"batt\":{\"present\":";
    j += power_has_battery() ? "true" : "false";
    j += ",\"pct\":" + String(power_battery_pct());
    j += ",\"charging\":";
    j += power_charging() ? "true" : "false";
    j += "}}";
    send_json(200, j);
}

static void handle_scan() {
    s_last_web = millis();
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        send_json(200, "{\"scanning\":true}");
        return;
    }
    if (n < 0 || millis() - s_scan_at > SCAN_MAX_AGE_MS) {
        WiFi.scanDelete();
        WiFi.scanNetworks(true);
        s_scan_at = millis();
        send_json(200, "{\"scanning\":true}");
        return;
    }
    // Several access points may share one SSID: keep the strongest.
    String j = "{\"nets\":[";
    bool first = true;
    for (int i = 0; i < n; ++i) {
        const String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        bool dup = false;
        for (int k = 0; k < i && !dup; ++k) dup = WiFi.SSID(k) == ssid && WiFi.RSSI(k) >= WiFi.RSSI(i);
        bool weaker_later = false;
        for (int k = i + 1; k < n && !weaker_later; ++k)
            weaker_later = WiFi.SSID(k) == ssid && WiFi.RSSI(k) > WiFi.RSSI(i);
        if (dup || weaker_later) continue;
        if (!first) j += ',';
        first = false;
        j += "{\"ssid\":";
        json_str(j, ssid.c_str());
        j += ",\"rssi\":" + String(WiFi.RSSI(i));
        j += ",\"open\":";
        j += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false";
        j += '}';
    }
    j += "]}";
    send_json(200, j);
}

static bool parse_hhmm(const String& s, uint16_t* out) {
    int h, m;
    if (sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = (uint16_t)(h * 60 + m);
    return true;
}

static bool valid_tz(const String& tz) {
    if (tz.length() == 0 || tz.length() >= sizeof(g_settings.tz)) return false;
    for (size_t i = 0; i < tz.length(); ++i) {
        const char c = tz[i];
        if (!isalnum((unsigned char)c) && !strchr("<>+-,./:", c)) return false;
    }
    return true;
}

// Host name or IPv4 address: letters, digits, '.', '-'; empty = slot unused.
static bool valid_host(const String& host) {
    if (host.length() >= sizeof(g_settings.ntp[0])) return false;
    for (size_t i = 0; i < host.length(); ++i) {
        const char c = host[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-') return false;
    }
    return true;
}

// `code` is a short ASCII error id; the page shows it in the chosen language.
static void fail(const char* code) {
    String j = "{\"ok\":false,\"error\":";
    json_str(j, code);
    j += '}';
    send_json(400, j);
}

static void handle_save() {
    s_last_web = millis();
    const String ssid = s_web.arg("ssid");
    const String pass = s_web.arg("pass");
    const String tz = s_web.arg("tz");
    uint16_t ns, ne, ss, se;
    if (ssid.length() == 0 || ssid.length() > 32) return fail("ssid");
    if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 64))
        return fail("pass");
    if (!valid_tz(tz)) return fail("tz");
    if (!parse_hhmm(s_web.arg("ns"), &ns) || !parse_hhmm(s_web.arg("ne"), &ne))
        return fail("night");
    if (!parse_hhmm(s_web.arg("ss"), &ss) || !parse_hhmm(s_web.arg("se"), &se))
        return fail("sleep");
    const long lday = s_web.arg("lday").toInt(), lnight = s_web.arg("lnight").toInt();
    const long rot = s_web.arg("rot").toInt();
    if (lday < 0 || lday >= BRIGHTNESS_LEVELS || lnight < 0 || lnight >= BRIGHTNESS_LEVELS)
        return fail("level");
    if (rot < 0 || rot > ROTATION_AUTO) return fail("rot");
    String ntp[NTP_SERVERS];
    bool any_ntp = false;
    for (int i = 0; i < NTP_SERVERS; ++i) {
        ntp[i] = s_web.arg(String("ntp") + (i + 1));
        ntp[i].trim();
        if (!valid_host(ntp[i])) return fail("ntp");
        any_ntp |= ntp[i].length() > 0;
    }

    // Empty password + same SSID keeps the stored password.
    const bool same_ssid = ssid == g_settings.ssid;
    const bool wifi_changed = !same_ssid || pass.length() > 0 || !s_connected;
    strncpy(g_settings.ssid, ssid.c_str(), sizeof(g_settings.ssid) - 1);
    g_settings.ssid[sizeof(g_settings.ssid) - 1] = 0;
    if (pass.length() > 0 || !same_ssid) {
        strncpy(g_settings.pass, pass.c_str(), sizeof(g_settings.pass) - 1);
        g_settings.pass[sizeof(g_settings.pass) - 1] = 0;
    }
    strncpy(g_settings.tz, tz.c_str(), sizeof(g_settings.tz) - 1);
    g_settings.h12 = s_web.arg("h12") == "1";
    g_settings.show_date = s_web.arg("date") == "1";
    g_settings.night_enabled = s_web.arg("night") == "1";
    g_settings.night_start = ns;
    g_settings.night_end = ne;
    g_settings.level_day = (uint8_t)lday;
    g_settings.level_night = (uint8_t)lnight;
    g_settings.rotation = (uint8_t)rot;
    g_settings.sleep_enabled = s_web.arg("sleep") == "1";
    g_settings.sleep_start = ss;
    g_settings.sleep_end = se;
    g_settings.swap_hourly = s_web.arg("swap") == "1";
    // All three empty restores the defaults (see settings sanitize()).
    for (int i = 0; i < NTP_SERVERS; ++i) {
        strncpy(g_settings.ntp[i], any_ntp ? ntp[i].c_str() : "", sizeof(g_settings.ntp[i]) - 1);
        g_settings.ntp[i][sizeof(g_settings.ntp[i]) - 1] = 0;
    }
    if (!settings_save()) return fail("nvs");
    timekeep_set_tz(g_settings.tz);
    const char* const servers[NTP_SERVERS] = {g_settings.ntp[0], g_settings.ntp[1], g_settings.ntp[2]};
    timekeep_set_servers(servers);
    if (wifi_changed) s_reconnect_pending = true;
    LOGI("net", "settings saved from web (wifi %s)", wifi_changed ? "changed" : "same");
    send_json(200, String("{\"ok\":true,\"wifi_changed\":") + (wifi_changed ? "true" : "false") + "}");
}

static void handle_not_found() {
    // In setup mode every unknown URL (captive-portal probes from iOS,
    // Android, Windows) is sent to the settings page.
    if (s_mode == MODE_SETUP) {
        s_web.sendHeader("Location", String("http://") + s_ap_ip + "/", true);
        s_web.send(302, "text/plain", "");
        return;
    }
    s_web.send(404, "text/plain", "not found");
}

// ------------------------------------------------------------------ setup mode

static void make_ap_identity() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    // Fresh 8-digit WPA2 password per session, shown on the panel and in the QR.
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
}

void net_start_setup() {
    if (s_mode == MODE_SETUP) return;
    make_ap_identity();
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(s_ap_ssid, s_ap_pass, 1, 0, 2)) LOGE("net", "softAP start failed");
    snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", WiFi.softAPIP().toString().c_str());
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    if (!s_dns.start(53, "*", WiFi.softAPIP())) LOGE("net", "DNS server start failed");
    s_mode = MODE_SETUP;
    s_phase = SETUP_WAIT;
    s_phase_at = s_last_web = millis();
    s_err[0] = 0;
    WiFi.scanNetworks(true);
    s_scan_at = millis();
    LOGI("net", "setup mode: AP \"%s\" at %s", s_ap_ssid, s_ap_ip);
}

void net_stop_setup() {
    if (s_mode != MODE_SETUP) return;
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_mode = MODE_STA;
    s_phase = SETUP_OFF;
    LOGI("net", "setup mode closed");
    if (!s_connected) {
        s_retry_at = millis();
        s_attempting = false;
    }
}

// ------------------------------------------------------------------ main

void net_init() {
    WiFi.persistent(false);   // credentials live in our own NVS namespace
    WiFi.setAutoReconnect(false);   // reconnects are scheduled in net_loop()
    WiFi.setHostname(HOSTNAME);
    // With an IPv6 link-local address mDNS can answer AAAA queries too.
    // Without it, systems that ask for A + AAAA (curl, Python, some browsers
    // on macOS) wait ~5 s for "clock.local" before using the IPv4 answer.
    WiFi.enableIPv6();
    WiFi.onEvent(on_wifi_event);
    WiFi.mode(WIFI_STA);

    s_web.on("/", HTTP_GET, handle_root);
    s_web.on("/api/state", HTTP_GET, handle_state);
    s_web.on("/api/scan", HTTP_GET, handle_scan);
    s_web.on("/api/settings", HTTP_POST, handle_save);
    s_web.onNotFound(handle_not_found);
    s_web.begin();

    // Without credentials, open the setup portal only when there is no time
    // to show; with a valid RTC time the clock just runs (hold BOOT for setup).
    if (settings_has_wifi()) begin_attempt();
    else if (!timekeep_valid()) net_start_setup();
    else LOGI("net", "no Wi-Fi configured; running from RTC (hold BOOT 3 s for setup)");
}

static void handle_events(uint32_t now) {
    if (s_ev_got_ip) {
        s_ev_got_ip = false;
        s_connected = true;
        s_attempting = false;
        s_leaving = false;
        s_backoff = WIFI_RETRY_MIN_MS;
        s_err[0] = 0;
        snprintf(s_ip, sizeof(s_ip), "%s", WiFi.localIP().toString().c_str());
        LOGI("net", "connected, IP %s, RSSI %d dBm", s_ip, WiFi.RSSI());
        timekeep_start_ntp();
        if (!s_mdns) {
            s_mdns = MDNS.begin(HOSTNAME);
            if (s_mdns) MDNS.addService("http", "tcp", 80);
            else LOGE("net", "mDNS start failed");
        }
        if (s_mode == MODE_SETUP && (s_phase == SETUP_CONNECTING || s_phase == SETUP_FAIL)) {
            s_phase = SETUP_OK;
            s_phase_at = now;
        }
    }
    if (s_ev_disc) {
        s_ev_disc = false;
        const uint8_t reason = s_ev_reason;
        if (s_leaving && reason == WIFI_REASON_ASSOC_LEAVE) {
            s_leaving = false;   // our own WiFi.disconnect(): not a failure
            LOGI("net", "left previous network");
            return;
        }
        if (s_connected) {
            ++s_disconnects;
            LOGI("net", "disconnected (reason %u)", reason);
        } else {
            LOGI("net", "connect failed (reason %u: %s)", reason, reason_text(reason));
        }
        s_connected = false;
        s_attempting = false;
        s_ip[0] = 0;
        snprintf(s_err, sizeof(s_err), "%s", reason_text(reason));
        if (s_mode == MODE_SETUP && s_phase == SETUP_CONNECTING) {
            s_phase = SETUP_FAIL;
            s_phase_at = now;
        } else {
            s_retry_at = now + s_backoff;
            s_backoff = min(s_backoff * 2, WIFI_RETRY_MAX_MS);
        }
    }
}

// Modem sleep saves ~tens of mA but makes the chip miss multicast between
// beacons, so "clock.local" (mDNS) and the web page answer unreliably. On USB
// power keep the radio awake; on battery let it sleep.
static void update_power_save(uint32_t now) {
    if (now - s_power_check_at < POWER_CHECK_MS && s_sleep_mode >= 0) return;
    s_power_check_at = now;
    const int8_t want = power_vbus() ? 0 : 1;
    if (want == s_sleep_mode) return;
    if (WiFi.setSleep(want == 1)) {
        s_sleep_mode = want;
        LOGI("net", "Wi-Fi power save %s (%s)", want ? "on" : "off", want ? "battery" : "USB power");
    }
}

void net_loop() {
    const uint32_t now = millis();
    handle_events(now);
    update_power_save(now);

    if (s_reconnect_pending) {
        s_reconnect_pending = false;
        s_leaving = s_connected || s_attempting;
        WiFi.disconnect(false);
        s_connected = false;
        s_ip[0] = 0;
        s_backoff = WIFI_RETRY_MIN_MS;
        begin_attempt();
        if (s_mode == MODE_SETUP) {
            s_phase = SETUP_CONNECTING;
            s_phase_at = now;
        }
    }

    if (s_attempting && now - s_attempt_at > ATTEMPT_TIMEOUT_MS) {
        s_attempting = false;
        snprintf(s_err, sizeof(s_err), "TIMEOUT");
        s_leaving = true;
        WiFi.disconnect(false);
        LOGI("net", "connect attempt timed out");
        if (s_mode == MODE_SETUP && s_phase == SETUP_CONNECTING) {
            s_phase = SETUP_FAIL;
            s_phase_at = now;
        } else {
            s_retry_at = now + s_backoff;
            s_backoff = min(s_backoff * 2, WIFI_RETRY_MAX_MS);
        }
    }

    if (s_mode == MODE_STA && !s_connected && !s_attempting && settings_has_wifi() &&
        (int32_t)(now - s_retry_at) >= 0) {
        begin_attempt();
    }

    if (s_mode == MODE_SETUP) {
        s_dns.processNextRequest();
        if (s_phase == SETUP_OK && now - s_phase_at > SETUP_SUCCESS_LINGER_MS) net_stop_setup();
        else if (s_phase != SETUP_CONNECTING && settings_has_wifi() && now - s_last_web > SETUP_IDLE_EXIT_MS)
            net_stop_setup();
    }
    s_web.handleClient();
}

bool net_in_setup() { return s_mode == MODE_SETUP; }
SetupPhase net_setup_phase() { return s_phase; }

const char* net_setup_message() {
    switch (s_phase) {
        case SETUP_WAIT: return "WAITING FOR SETTINGS...";
        case SETUP_CONNECTING: return "CONNECTING...";
        case SETUP_OK: return "CONNECTED";
        case SETUP_FAIL: return s_err[0] ? s_err : "CONNECT FAILED";
        default: return "";
    }
}

bool net_connected() { return s_connected; }
const char* net_ip() { return s_ip; }
int net_rssi() { return s_connected ? WiFi.RSSI() : 0; }
const char* net_ap_ssid() { return s_ap_ssid; }
const char* net_ap_pass() { return s_ap_pass; }
const char* net_ap_ip() { return s_ap_ip; }
const char* net_last_error() { return s_err; }
uint32_t net_disconnects() { return s_disconnects; }
