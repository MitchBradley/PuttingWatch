#include "Imu.h"
#include "SensorQMI8658.hpp"

namespace {
constexpr float DEG2RAD = 0.01745329252f;
SensorQMI8658 qmi;
}  // namespace

bool Imu::begin(TwoWire& wire, uint8_t addr) {
    ok_ = qmi.begin(wire, addr);
    if (!ok_) return false;

    // +-4 g is ample headroom for a putting stroke (peaks well under 1 g of
    // linear accel); 500 Hz ODR lets the 100 Hz sampling task always find a
    // fresh sample without polling hard.
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_500Hz,
                            SensorQMI8658::LPF_MODE_0);
    // +-512 dps: a fast wristy stroke can spike a few hundred dps about the
    // shaft; this keeps margin without throwing away resolution.
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_512DPS,
                        SensorQMI8658::GYR_ODR_448_4Hz,
                        SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
    qmi.enableGyroscope();
    return true;
}

bool Imu::read(Vec3& accel_g, Vec3& gyro_rps) {
    if (!ok_ || !qmi.getDataReady()) return false;

    float ax, ay, az, gx, gy, gz;
    if (!qmi.getAccelerometer(ax, ay, az)) return false;
    if (!qmi.getGyroscope(gx, gy, gz)) return false;  // deg/s

    accel_g = Vec3{ax, ay, az};
    gyro_rps = Vec3{gx * DEG2RAD, gy * DEG2RAD, gz * DEG2RAD};
    return true;
}
