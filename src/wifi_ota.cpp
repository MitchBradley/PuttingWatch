#include "wifi_ota.h"

#include <DNSServer.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <SHA2Builder.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <time.h>
#include "HWCDC.h"
#include "UpdateCore.h"
#include "update_config.h"

extern HWCDC USBSerial;
#define LOGF(fmt, ...) USBSerial.printf("[ota %lu] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

// The CA certificate bundle (full Mozilla list) that the Arduino core links
// into every build. It MUST be handed to the TLS client explicitly:
// HTTPClient::begin(url) with no CA silently falls back to setInsecure(),
// i.e. no certificate checking at all.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace wifiota {
namespace {

constexpr uint32_t kJoinTimeoutMs = 20000;
constexpr uint32_t kPortalTimeoutMs = 10 * 60 * 1000;  // give up on setup after 10 minutes
constexpr uint32_t kHttpTimeoutMs = 10000;
constexpr uint32_t kStallMs = 15000;  // no download bytes for this long = stalled
constexpr size_t kChunk = 4096;
constexpr int kMaxNetworks = 24;
constexpr time_t kClockSane = 1700000000;  // Nov 2023: any real clock reads later than this

const char* gRunningVersion = "";
Status gStatus;
SemaphoreHandle_t gMux = nullptr;
volatile int gBatteryPct = -1;
volatile bool gExternalPower = false;

enum class Cmd : uint8_t { None, Install, Retry, ChangeWifi };
volatile Cmd gCmd = Cmd::None;

updatecore::Manifest gManifest;
char gJoinNote[96] = "";  // why the last join attempt failed; shown on the Provisioning screen

// ---- shared status --------------------------------------------------------

template <typename F>
void modify(F f) {
    xSemaphoreTake(gMux, portMAX_DELAY);
    f(gStatus);
    xSemaphoreGive(gMux);
}

void setPhase(Phase p) {
    modify([&](Status& s) { s.phase = p; });
    LOGF("phase %d (heap %u)", (int)p, (unsigned)ESP.getFreeHeap());
}

void setError(const char* msg) {
    modify([&](Status& s) {
        s.phase = Phase::Error;
        snprintf(s.error, sizeof(s.error), "%s", msg);
    });
    LOGF("error: %s", msg);
}

Phase currentPhase() {
    xSemaphoreTake(gMux, portMAX_DELAY);
    Phase p = gStatus.phase;
    xSemaphoreGive(gMux);
    return p;
}

// ---- saved Wi-Fi credentials ----------------------------------------------

bool loadCreds(String& ssid, String& pass) {
    Preferences p;
    if (!p.begin("wifi", false)) return false;
    ssid = p.getString("ssid", "");
    pass = p.getString("pass", "");
    p.end();
    return ssid.length() > 0;
}

void saveCreds(const String& ssid, const String& pass) {
    Preferences p;
    if (!p.begin("wifi", false)) return;
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}

// ---- joining a network ----------------------------------------------------

enum class JoinResult { Ok, NotFound, BadPassword, Timeout };

JoinResult staConnect(const char* ssid, const char* pass, uint32_t timeoutMs) {
    LOGF("joining '%s'", ssid);
    WiFi.persistent(false);  // we keep our own credentials; don't write the driver's copy
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // radio sleep halves download speed
    WiFi.disconnect();
    WiFi.begin(ssid, (pass && *pass) ? pass : nullptr);

    // The driver retries on its own, so the failure status flickers rather
    // than sticking - remember that we saw it.
    bool sawNoSsid = false, sawFailed = false;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        switch (WiFi.status()) {
            case WL_CONNECTED:
                LOGF("joined, ip %s", WiFi.localIP().toString().c_str());
                return JoinResult::Ok;
            case WL_NO_SSID_AVAIL: sawNoSsid = true; break;
            case WL_CONNECT_FAILED: sawFailed = true; break;
            default: break;
        }
        if (sawFailed && millis() - t0 > 4000) break;  // wrong password: no point waiting
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    WiFi.disconnect();
    if (sawFailed) return JoinResult::BadPassword;
    if (sawNoSsid) return JoinResult::NotFound;
    return JoinResult::Timeout;
}

void describeJoinFailure(JoinResult r, const char* ssid, char* out, size_t n) {
    switch (r) {
        case JoinResult::NotFound: snprintf(out, n, "Couldn't find \"%s\".", ssid); break;
        case JoinResult::BadPassword: snprintf(out, n, "Couldn't join \"%s\". Wrong password?", ssid); break;
        default: snprintf(out, n, "Couldn't connect to \"%s\".", ssid); break;
    }
}

// ---- scanning -------------------------------------------------------------

struct Net {
    char ssid[33];
    bool open;
    int rssi;
};
Net gNets[kMaxNetworks];
int gNetCount = 0;

// Blocking, a couple of seconds. Done BEFORE the setup access point comes up:
// scanning from an AP forces it off its channel and drops the phone.
void scanNetworks() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    int n = WiFi.scanNetworks(false /*async*/, false /*hidden*/);
    gNetCount = 0;
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0 || s.length() > 32) continue;
        int slot = -1;
        for (int j = 0; j < gNetCount; j++)
            if (s == gNets[j].ssid) slot = j;
        if (slot < 0) {
            if (gNetCount >= kMaxNetworks) continue;
            slot = gNetCount++;
            snprintf(gNets[slot].ssid, sizeof(gNets[slot].ssid), "%s", s.c_str());
            gNets[slot].rssi = -127;
        }
        if (WiFi.RSSI(i) > gNets[slot].rssi) {
            gNets[slot].rssi = WiFi.RSSI(i);
            gNets[slot].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        }
    }
    WiFi.scanDelete();
    for (int i = 1; i < gNetCount; i++) {  // strongest first
        Net t = gNets[i];
        int j = i - 1;
        while (j >= 0 && gNets[j].rssi < t.rssi) { gNets[j + 1] = gNets[j]; j--; }
        gNets[j + 1] = t;
    }
    LOGF("scan: %d networks", gNetCount);
}

// ---- the captive portal ---------------------------------------------------

const char kPageHead[] =
    "<!doctype html><html lang=en><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>PuttingWatch Wi-Fi</title><style>"
    ":root{color-scheme:light dark}"
    "body{font:18px/1.45 system-ui,sans-serif;max-width:30rem;margin:0 auto;padding:20px}"
    "h1{font-size:1.45rem;margin:.2rem 0 .6rem}"
    "label{display:block;margin:1.1rem 0 .3rem;font-weight:600}"
    "select,input[type=text],input[type=password],button{width:100%;box-sizing:border-box;"
    "font:inherit;padding:.75rem;border-radius:.6rem;border:1px solid #8888}"
    "button{background:#1b7f3b;color:#fff;border:0;margin-top:1.6rem;font-weight:600}"
    ".note{color:#888;font-size:.9rem}"
    ".show{display:flex;gap:.5rem;align-items:center;font-weight:400;margin-top:.6rem}"
    ".show input{width:auto}.err{color:#c0392b;font-weight:600}"
    "</style></head><body>";

String portalPage() {
    String h(kPageHead);
    h += "<h1>Connect your watch to Wi-Fi</h1>"
         "<p class=note>The watch needs your Wi-Fi to download updates. "
         "It can only use 2.4&nbsp;GHz networks.</p>"
         "<form method=post action=/connect>"
         "<label for=n>Wi-Fi network</label>"
         "<select id=n name=ssid onchange=\"document.getElementById('o').style.display="
         "this.value=='__other'?'block':'none'\">";
    for (int i = 0; i < gNetCount; i++) {
        std::string e = updatecore::htmlEscape(gNets[i].ssid);
        h += "<option value=\"";
        h += e.c_str();
        h += "\">";
        h += e.c_str();
        h += gNets[i].open ? " (open)" : "";
        h += "</option>";
    }
    h += "<option value=\"__other\">Other / hidden network...</option></select>";
    h += "<input type=text id=o name=other placeholder=\"Network name\" autocapitalize=off "
         "autocorrect=off style=\"margin-top:.5rem;display:";
    h += gNetCount == 0 ? "block" : "none";
    h += "\"><label for=p>Password</label>"
         "<input type=password id=p name=pass autocomplete=off autocapitalize=off>"
         "<label class=show><input type=checkbox onclick=\"document.getElementById('p').type="
         "this.checked?'text':'password'\"> Show password</label>"
         "<button type=submit>Connect</button></form></body></html>";
    return h;
}

String messagePage(const char* title, const String& bodyHtml, bool isError) {
    String h(kPageHead);
    h += "<h1>";
    h += title;
    h += "</h1><p";
    h += isError ? " class=err" : "";
    h += ">";
    h += bodyHtml;
    h += "</p>";
    if (isError) h += "<p><a href=\"/\">Go back</a></p>";
    h += "</body></html>";
    return h;
}

struct Portal {
    WebServer server;
    DNSServer dns;
    volatile bool submitted = false;
    String ssid, pass;
    Portal() : server(80) {}
};

void handleConnect(Portal& p) {
    String ssid = p.server.arg("ssid");
    if (ssid == "__other") ssid = p.server.arg("other");
    ssid.trim();
    String pass = p.server.arg("pass");

    const char* err = nullptr;
    if (ssid.length() == 0 || ssid.length() > 32)
        err = "Please choose a network, or type its name.";
    else if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63))
        err = "A Wi-Fi password is between 8 and 63 characters.";
    if (err) {
        p.server.send(400, "text/html", messagePage("Please check", err, true));
        return;
    }

    p.ssid = ssid;
    p.pass = pass;
    p.submitted = true;
    String body = "The watch is now joining <b>";
    body += updatecore::htmlEscape(ssid.c_str()).c_str();
    body += "</b>. You can close this page; the result shows on the watch.";
    p.server.send(200, "text/html", messagePage("Check your watch", body, false));
}

// ---- HTTPS ----------------------------------------------------------------

// Start a verified HTTPS GET. Returns the HTTP status, or a negative
// HTTPClient error. (Plain HTTP URLs are refused upstream: the manifest
// parser only accepts https:// for the firmware URL, and OTA_SITE_URL is https.)
int beginGet(HTTPClient& http, NetworkClientSecure& client, const char* url) {
    client.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
    client.setHandshakeTimeout(15);
    if (!http.begin(client, url)) return -100;
    // HTTP/1.0: the server must send Content-Length rather than chunked
    // encoding, which the streaming download below relies on.
    http.useHTTP10(true);
    http.setUserAgent(String("PuttingWatch/") + gRunningVersion);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    return http.GET();
}

void setHttpError(int code, NetworkClientSecure& client) {
    char b[96];
    if (code > 0) {
        snprintf(b, sizeof(b), "The update server returned error %d.", code);
    } else {
        char tls[48] = "";
        if (client.lastError(tls, sizeof(tls)) == 0) tls[0] = 0;
        snprintf(b, sizeof(b), "Couldn't reach the update server.\n%s",
                 tls[0] ? tls : HTTPClient::errorToString(code).c_str());
    }
    setError(b);
}

bool syncClock() {
    if (time(nullptr) >= kClockSane) return true;
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    uint32_t t0 = millis();
    while (time(nullptr) < kClockSane && millis() - t0 < 15000) vTaskDelay(pdMS_TO_TICKS(200));
    return time(nullptr) >= kClockSane;
}

// ---- the engine's states ---------------------------------------------------

enum class Next { Connect, Provision, Check, Download, Idle };

Next doConnect() {
    String ssid, pass;
    if (!loadCreds(ssid, pass)) {
        gJoinNote[0] = 0;
        return Next::Provision;
    }
    modify([&](Status& s) {
        s.phase = Phase::Connecting;
        snprintf(s.ssid, sizeof(s.ssid), "%s", ssid.c_str());
        s.error[0] = 0;
    });
    JoinResult r = staConnect(ssid.c_str(), pass.c_str(), kJoinTimeoutMs);
    if (r == JoinResult::Ok) return Next::Check;
    describeJoinFailure(r, ssid.c_str(), gJoinNote, sizeof(gJoinNote));
    return Next::Provision;
}

Next doProvision() {
    setPhase(Phase::Scanning);
    scanNetworks();

    uint8_t rnd[64];
    esp_fill_random(rnd, sizeof(rnd));
    std::string pass = updatecore::makePassword(rnd, sizeof(rnd));
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char apSsid[24];
    snprintf(apSsid, sizeof(apSsid), "PuttingWatch-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    // WPA2 with a one-time password that travels in the QR code: the home
    // Wi-Fi password is typed into a page served over this link, so it must
    // not be an open network.
    if (!WiFi.softAP(apSsid, pass.c_str(), 6, 0, 2)) {
        setError("Couldn't start the watch's Wi-Fi setup network.");
        return Next::Idle;
    }

    Portal* portal = new Portal();
    portal->dns.setErrorReplyCode(DNSReplyCode::NoError);
    portal->dns.start(53, "*", WiFi.softAPIP());  // every name -> the watch: triggers the phone's sign-in sheet
    portal->server.on("/", HTTP_GET, [portal]() {
        portal->server.sendHeader("Cache-Control", "no-store");
        portal->server.send(200, "text/html", portalPage());
    });
    portal->server.on("/connect", HTTP_POST, [portal]() { handleConnect(*portal); });
    // Connectivity probes (/generate_204, /hotspot-detect.html, /connecttest.txt, ...)
    // and anything else: send the phone to the page.
    portal->server.onNotFound([portal]() {
        portal->server.sendHeader("Location", "http://192.168.4.1/", true);
        portal->server.send(302, "text/plain", "");
    });
    portal->server.begin();

    modify([&](Status& s) {
        s.phase = Phase::Provisioning;
        s.portalClient = false;
        snprintf(s.apSsid, sizeof(s.apSsid), "%s", apSsid);
        snprintf(s.apPass, sizeof(s.apPass), "%s", pass.c_str());
        snprintf(s.error, sizeof(s.error), "%s", gJoinNote);
    });
    LOGF("setup AP '%s' up, portal at http://192.168.4.1/", apSsid);

    bool submitted = false, hadClient = false;
    uint32_t t0 = millis();
    while (millis() - t0 < kPortalTimeoutMs) {
        portal->server.handleClient();
        bool client = WiFi.softAPgetStationNum() > 0;
        if (client != hadClient) {
            hadClient = client;
            modify([&](Status& s) { s.portalClient = client; });
        }
        if (portal->submitted) {
            submitted = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (submitted) {  // let the "check your watch" reply reach the phone before the AP goes
        uint32_t t1 = millis();
        while (millis() - t1 < 1500) {
            portal->server.handleClient();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    String ssid = portal->ssid, wifiPass = portal->pass;
    portal->server.stop();
    portal->dns.stop();
    delete portal;
    WiFi.softAPdisconnect(true);

    if (!submitted) {
        setError("Wi-Fi setup timed out.");
        return Next::Idle;
    }

    modify([&](Status& s) {
        s.phase = Phase::Connecting;
        snprintf(s.ssid, sizeof(s.ssid), "%s", ssid.c_str());
        s.error[0] = 0;
    });
    JoinResult r = staConnect(ssid.c_str(), wifiPass.c_str(), kJoinTimeoutMs);
    if (r == JoinResult::Ok) {
        saveCreds(ssid, wifiPass);
        gJoinNote[0] = 0;
        return Next::Check;
    }
    describeJoinFailure(r, ssid.c_str(), gJoinNote, sizeof(gJoinNote));
    return Next::Provision;  // back to a fresh QR, with the reason shown
}

Next doCheck() {
    setPhase(Phase::Checking);
    if (WiFi.status() != WL_CONNECTED) return Next::Connect;
    if (!syncClock()) {
        setError("Couldn't get the time from the internet.\nIs this Wi-Fi connected to the internet?");
        return Next::Idle;
    }

    NetworkClientSecure client;
    HTTPClient http;
    int code = beginGet(http, client, OTA_MANIFEST_URL);
    if (code != 200) {
        setHttpError(code, client);
        http.end();
        return Next::Idle;
    }
    int len = http.getSize();
    if (len < 0 || len > 4096) {
        setError("Unexpected reply from the update server.");
        http.end();
        return Next::Idle;
    }
    String body = http.getString();
    http.end();

    updatecore::Manifest m;
    switch (updatecore::parseManifest(body.c_str(), m)) {
        case updatecore::ManifestError::None: break;
        case updatecore::ManifestError::UnsupportedSchema:
            setError("This update needs a newer updater.\nPlease install it with a USB cable.");
            return Next::Idle;
        default:
            setError("The update information on the server is unreadable. Please try again later.");
            return Next::Idle;
    }

    gManifest = m;
    LOGF("manifest: version %s size %u (running %s)", m.version.c_str(), (unsigned)m.size, gRunningVersion);
    Phase result = Phase::UpToDate;
    switch (updatecore::decide(gRunningVersion, m)) {
        case updatecore::Decision::Available: result = Phase::Available; break;
        case updatecore::Decision::NeedsUsbInstall: result = Phase::NeedsUsbInstall; break;
        default: break;
    }
    modify([&](Status& s) {
        snprintf(s.version, sizeof(s.version), "%s", m.version.c_str());
        snprintf(s.notes, sizeof(s.notes), "%s", m.notes.c_str());
        s.phase = result;
    });
    LOGF("phase %d", (int)result);
    return Next::Idle;
}

Next doDownload() {
    const updatecore::Manifest m = gManifest;
    if (!updatecore::batteryAllowsUpdate(gBatteryPct, gExternalPower)) {
        setError("The battery is too low to update safely.\nPlug in a charger and try again.");
        return Next::Idle;
    }
    if (WiFi.status() != WL_CONNECTED) {
        setError("The Wi-Fi connection was lost.");
        return Next::Idle;
    }
    modify([&](Status& s) {
        s.phase = Phase::Downloading;
        s.progressPct = 0;
    });
    LOGF("downloading %s (%u bytes)", m.url.c_str(), (unsigned)m.size);

    NetworkClientSecure client;
    HTTPClient http;
    int code = beginGet(http, client, m.url.c_str());
    if (code != 200) {
        setHttpError(code, client);
        http.end();
        return Next::Idle;
    }
    if (http.getSize() != (int)m.size) {
        setError("The update file isn't the size the server promised.");
        http.end();
        return Next::Idle;
    }
    if (!Update.begin(m.size)) {
        setError(Update.errorString());
        http.end();
        return Next::Idle;
    }
    uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk));
    if (!buf) {
        Update.abort();
        setError("Out of memory.");
        http.end();
        return Next::Idle;
    }

    SHA256Builder sha;
    sha.begin();
    NetworkClient* stream = http.getStreamPtr();
    char fail[96] = "";
    uint32_t written = 0, lastData = millis();
    uint8_t lastPct = 0;
    while (written < m.size && !fail[0]) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!stream->connected())
                snprintf(fail, sizeof(fail), "The download was interrupted.");
            else if (millis() - lastData > kStallMs)
                snprintf(fail, sizeof(fail), "The download stalled. Check the Wi-Fi signal.");
            else
                vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t want = min(min(avail, kChunk), static_cast<size_t>(m.size - written));
        int n = stream->readBytes(buf, want);
        if (n <= 0) continue;
        if (Update.write(buf, n) != static_cast<size_t>(n)) {
            snprintf(fail, sizeof(fail), "%s", Update.errorString());
            continue;
        }
        sha.add(buf, n);
        written += n;
        lastData = millis();
        uint8_t pct = static_cast<uint8_t>(static_cast<uint64_t>(written) * 100 / m.size);
        if (pct != lastPct) {
            lastPct = pct;
            modify([&](Status& s) { s.progressPct = pct; });
        }
    }
    free(buf);
    http.end();

    if (!fail[0]) {
        sha.calculate();
        if (!sha.toString().equalsIgnoreCase(m.sha256.c_str()))
            snprintf(fail, sizeof(fail), "The downloaded update was corrupted. Please try again.");
    }
    if (fail[0]) {
        Update.abort();
        setError(fail);
        return Next::Idle;
    }
    if (!Update.end()) {  // checks the image's own header/hash, then selects it for the next boot
        char msg[96];
        snprintf(msg, sizeof(msg), "%s", Update.errorString());
        setError(msg);
        return Next::Idle;
    }

    setPhase(Phase::Installed);
    LOGF("installed %s - restarting", m.version.c_str());
    USBSerial.flush();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return Next::Idle;
}

Next waitForCommand() {
    for (;;) {
        Cmd c = gCmd;
        if (c != Cmd::None) {
            gCmd = Cmd::None;
            switch (c) {
                case Cmd::Install:
                    if (currentPhase() == Phase::Available) return Next::Download;
                    break;
                case Cmd::Retry:
                    return WiFi.status() == WL_CONNECTED ? Next::Check : Next::Connect;
                case Cmd::ChangeWifi:
                    gJoinNote[0] = 0;
                    return Next::Provision;
                default: break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void taskMain(void*) {
    Next next = Next::Connect;
    for (;;) {
        switch (next) {
            case Next::Connect: next = doConnect(); break;
            case Next::Provision: next = doProvision(); break;
            case Next::Check: next = doCheck(); break;
            case Next::Download: next = doDownload(); break;
            case Next::Idle: next = waitForCommand(); break;
        }
    }
}

}  // namespace

void begin(const char* runningVersion) {
    gRunningVersion = runningVersion ? runningVersion : "";
    gMux = xSemaphoreCreateMutex();
    LOGF("starting (running %s, manifest %s)", gRunningVersion, OTA_MANIFEST_URL);
    // 16 KB: the TLS handshake runs on this task's stack and is the deepest thing it does.
    xTaskCreate(taskMain, "wifiota", 16384, nullptr, 1, nullptr);
}

Status status() {
    xSemaphoreTake(gMux, portMAX_DELAY);
    Status s = gStatus;
    xSemaphoreGive(gMux);
    return s;
}

void setPower(int batteryPct, bool externalPower) {
    gBatteryPct = batteryPct;
    gExternalPower = externalPower;
}

void install() { gCmd = Cmd::Install; }
void retry() { gCmd = Cmd::Retry; }
void changeWifi() { gCmd = Cmd::ChangeWifi; }

}  // namespace wifiota
