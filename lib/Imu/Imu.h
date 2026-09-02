#pragma once
#include <Wire.h>
#include "Vec3.h"

// Thin wrapper over the QMI8658C 6-axis IMU (lewisxhe/SensorLib). Hands back
// raw accel (in g, gravity NOT removed) and gyro (in rad/s) in the sensor
// body frame. The gravity estimate and the MotionSample are assembled by the
// caller (main), which owns the GravityFilter - this class is just the
// driver.
//
// The QMI8658 shares the 400 kHz I2C bus with the touch panel, RTC and PMU;
// call Wire.begin(IIC_SDA, IIC_SCL) once before begin().
class Imu {
  public:
    bool begin(TwoWire& wire, uint8_t addr);

    // True when a fresh sample was read into accel_g / gyro_rps.
    bool read(Vec3& accel_g, Vec3& gyro_rps);

    bool ok() const { return ok_; }

  private:
    bool ok_ = false;
};
