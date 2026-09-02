#include "Battery.h"
#include "pin_config.h"  // defines XPOWERS_CHIP_AXP2101 before the lib header
#include <XPowersLib.h>

namespace {
XPowersPMU pmu;  // == XPowersAXP2101
}

bool Battery::begin(TwoWire& wire) {
    ok_ = pmu.begin(wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!ok_) return false;
    // The percentage register needs the battery ADC channels running.
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
    return true;
}

int Battery::percent() {
    if (!ok_ || !pmu.isBatteryConnect()) return -1;
    int p = pmu.getBatteryPercent();
    if (p < 0) return -1;
    return p > 100 ? 100 : p;
}
