#pragma once
#include <Wire.h>

// Thin wrapper over the AXP2101 PMU (lewisxhe/XPowersLib) for a battery
// readout, plus (on boards that wire a button to PWRON) the one write that
// does touch a power rail: shutdown().
//
// The AXP2101's own fuel-gauge percentage (getBatteryPercent) proved
// worthless on this board - it read 30% at 4.19 V - so we ignore it and
// derive percentage from the terminal voltage instead (percentForMillivolts).
class Battery {
  public:
    bool begin(TwoWire& wire);

    // Battery terminal voltage in millivolts, or -1 if unavailable. Does an
    // I2C read; call it at a low rate (a few seconds), not every frame.
    int millivolts();

    // Rough state of charge from a rested-LiPo OCV curve. Pure function -
    // pass in a (smoothed) millivolts reading. Returns 0..100, or -1 for
    // mv <= 0. Note: terminal voltage sags under load, so this reads a few
    // points low while the display is drawing and recovers when it idles.
    static int percentForMillivolts(int mv);

    // Tell the AXP2101 to cut every rail but VRTC - the ESP32's own supply
    // included. Does not return: the MCU loses power before the call site's
    // next line runs. Only the PMU's own PWRON pin (a physical button wired
    // to it in hardware, board-dependent) can bring the rails back, which
    // then looks like a normal cold boot. Call only when ok().
    void shutdown();

    bool ok() const { return ok_; }

  private:
    bool ok_ = false;
};
