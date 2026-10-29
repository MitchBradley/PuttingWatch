#pragma once

class Battery;

// "Update mode": a separate boot of the firmware (see main.cpp) that skips the
// putting app entirely and runs only the Wi-Fi setup + over-the-air update,
// with its own screens. Rebooting into it - rather than running it alongside
// the swing tracker - gives the TLS download all the RAM and none of the
// competing tasks, and leaves no way to end up half-way between the two.
//
// The screen is a view over wifiota::status() (src/wifi_ota.*); the network
// work itself runs in that module's own task.
namespace updateui {

// Build the screens on the active LVGL screen and start the update engine.
// `runningVersion` is this firmware's version string; it must outlive the call.
void begin(const char* runningVersion, Battery& battery);

// Call from loop() every iteration while in update mode.
void loop();

}  // namespace updateui
