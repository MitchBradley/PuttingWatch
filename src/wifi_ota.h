#pragma once
#include <Arduino.h>

// Wi-Fi setup and over-the-air firmware update, for "update mode" (see
// main.cpp / update_ui.cpp). Runs as its own FreeRTOS task of plain sequential
// code; the screen polls status() and sends commands, so neither blocks the
// other. All decisions (is this newer, is the manifest valid, is the battery
// enough) live in lib/UpdateCore, which is unit-tested on the host.
//
// The flow, in order:
//   no saved Wi-Fi  -> Scanning -> Provisioning: the watch becomes an access
//        point and a captive-portal page on the user's phone collects the home
//        network's name and password (the screen shows a QR code that joins the
//        phone to the AP, then one that opens the page) -> Connecting
//   saved Wi-Fi     -> Connecting
//   -> Checking     (set the clock via NTP, fetch + verify manifest.json over
//                    HTTPS) -> UpToDate | Available | NeedsUsbInstall
//   Available + install() -> Downloading (stream to the inactive OTA slot,
//                    SHA-256 checked against the manifest) -> Installed -> reboot
// Anything that fails lands in Error; retry() or changeWifi() leave it.
//
// Safe to cancel at any moment by restarting: the running firmware is only
// replaced when the very last step (Update.end) succeeds.
namespace wifiota {

enum class Phase : uint8_t {
    Starting,
    Scanning,         // looking for networks before opening the setup AP
    Provisioning,     // AP + captive portal up, waiting for the phone
    Connecting,       // joining a Wi-Fi network
    Checking,         // clock + manifest
    UpToDate,
    Available,        // a newer version can be installed
    NeedsUsbInstall,  // newer, but the running firmware is too old to update over the air
    Downloading,
    Installed,        // written; the watch is about to reboot into it
    Error,
};

struct Status {
    Phase phase = Phase::Starting;
    uint8_t progressPct = 0;   // Downloading
    bool portalClient = false; // Provisioning: a phone has joined the watch's AP
    char apSsid[24] = "";      // Provisioning: the setup network's name...
    char apPass[16] = "";      // ...and its one-time password
    char ssid[34] = "";        // Connecting: the network being joined
    char version[24] = "";     // UpToDate/Available/NeedsUsbInstall: the published version
    char notes[104] = "";      // ...and its one-line summary
    // Error: what went wrong. Provisioning: why the previous join attempt
    // failed, if it did (empty otherwise).
    char error[96] = "";
};

// Start the engine task. `runningVersion` is this firmware's version string
// (it must outlive the call - pass a string literal / macro).
void begin(const char* runningVersion);

Status status();  // a consistent copy; cheap enough to call every frame

// Latest battery state, fed by the screen. Install is refused on a low battery
// that isn't being charged.
void setPower(int batteryPct, bool externalPower);

void install();     // Available -> Downloading
void retry();       // Error -> reconnect and check again
void changeWifi();  // -> Provisioning, to pick a different network

}  // namespace wifiota
