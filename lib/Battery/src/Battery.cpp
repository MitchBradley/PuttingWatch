#include "Battery.h"
#include "pin_config.h"  // defines XPOWERS_CHIP_AXP2101 before the lib header
#include <XPowersLib.h>

namespace {
XPowersPMU pmu;  // == XPowersAXP2101

// Rested single-cell LiPo open-circuit-voltage curve, (mV, %) knots. Top
// knot lowered from the nominal 4200 to 4115 - this board's battery/charger
// tops out there in practice, never actually reaching 4200. Everything below
// is untouched pending real discharge data.
struct Knot { int mv, pct; };
const Knot kCurve[] = {
    {4115, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 45},
    {3750, 35},  {3700, 25}, {3650, 15}, {3550, 8},  {3450, 3}, {3300, 0},
};
}  // namespace

bool Battery::begin(TwoWire& wire) {
    ok_ = pmu.begin(wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (!ok_) return false;
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
    return true;
}

int Battery::millivolts() {
    if (!ok_ || !pmu.isBatteryConnect()) return -1;
    return (int)pmu.getBattVoltage();
}

bool Battery::externalPower() {
    return ok_ && pmu.isVbusIn();
}

void Battery::shutdown() {
    if (!ok_) return;
    pmu.shutdown();
}

int Battery::percentForMillivolts(int mv) {
    if (mv <= 0) return -1;
    if (mv >= kCurve[0].mv) return 100;
    const int n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (mv <= kCurve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= kCurve[i].mv) {
            const Knot& hi = kCurve[i - 1];
            const Knot& lo = kCurve[i];
            return lo.pct + (hi.pct - lo.pct) * (mv - lo.mv) / (hi.mv - lo.mv);
        }
    }
    return 0;
}
