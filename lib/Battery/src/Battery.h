#pragma once
#include <Wire.h>

// Thin wrapper over the AXP2101 PMU (lewisxhe/XPowersLib) for a battery
// percentage readout. Read-only: it never touches a power rail, just the
// fuel-gauge / ADC registers. Shares Wire (I2C bus 0) with everything else.
class Battery {
  public:
    bool begin(TwoWire& wire);

    // 0..100, or -1 if no battery / not readable yet. Does an I2C read; call
    // it at a low rate (a few seconds), not every frame.
    int percent();

    bool ok() const { return ok_; }

  private:
    bool ok_ = false;
};
