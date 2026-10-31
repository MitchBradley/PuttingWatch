// Standalone QMI8658 hardware probe. Nothing else runs: no UI, no audio,
// no display, no touch, no sampling task - just Wire + SensorLib.
//
//   pio run -e imuprobe -t upload -t monitor
//
// Tells us whether the IMU itself is healthy, independent of the rest of
// the firmware.

#include <Arduino.h>
#include <Wire.h>
#include "SensorQMI8658.hpp"

// Defaults are the S3 board's touch/IMU I2C pins; the C6 board wires the
// same bus to different GPIOs, overridden via build_flags in its env.
#ifndef PROBE_PIN_SDA
#define PROBE_PIN_SDA 15
#endif
#ifndef PROBE_PIN_SCL
#define PROBE_PIN_SCL 14
#endif
static constexpr int PIN_SDA = PROBE_PIN_SDA;
static constexpr int PIN_SCL = PROBE_PIN_SCL;

SensorQMI8658 qmi;

static void i2cScan() {
    Serial.print("I2C devices:");
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.printf(" 0x%02X", a);
            found++;
        }
    }
    Serial.printf("   (%d found)\n", found);
}

// Direct WHO_AM_I read, bypassing SensorLib entirely.
static int rawWhoAmI(uint8_t addr) {
    Wire.beginTransmission(addr);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((int)addr, 1) != 1) return -2;
    return Wire.read();
}

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    delay(400);
    Serial.println("\n\n===== QMI8658 IMU PROBE =====");

    Wire.begin(PIN_SDA, PIN_SCL, 400000);
    delay(20);
    i2cScan();
    Serial.printf("raw WHO_AM_I @0x6B = 0x%02X   @0x6A = 0x%02X   (expect 0x05)\n",
                     rawWhoAmI(0x6B) & 0xFF, rawWhoAmI(0x6A) & 0xFF);

    uint8_t addr = 0x6B;
    bool ok = qmi.begin(Wire, addr);
    if (!ok) {
        addr = 0x6A;
        ok = qmi.begin(Wire, addr);
    }
    Serial.printf("qmi.begin() @0x%02X = %d\n", addr, ok);
    if (!ok) {
        Serial.println("PROBE: begin failed - IMU not responding.");
        return;
    }
    Serial.printf("chipID = 0x%02X   whoAmI = 0x%02X\n", qmi.getChipID(), qmi.whoAmI());

    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_250Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_512DPS,
                        SensorQMI8658::GYR_ODR_224_2Hz,
                        SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
    qmi.enableGyroscope();

    Serial.println("--- CTRL register dump ---");
    qmi.dumpCtrlRegister();
    Serial.println("--- streaming (raw counts | g / dps), watch STILL on the desk ---");
}

void loop() {
    static uint32_t n = 0, lastPrint = 0;
    if (qmi.getDataReady()) {
        int16_t ar[3] = {0, 0, 0}, gr[3] = {0, 0, 0};
        bool a = qmi.getAccelRaw(ar);
        bool g = qmi.getGyroRaw(gr);
        n++;
        if (millis() - lastPrint >= 200) {
            lastPrint = millis();
            float ax, ay, az, gx, gy, gz;
            qmi.getAccelerometer(ax, ay, az);
            qmi.getGyroscope(gx, gy, gz);
            Serial.printf(
                "n=%lu a(%d) raw[%6d %6d %6d] = [% .3f % .3f % .3f]g   "
                "g(%d) raw[%6d %6d %6d] = [% .1f % .1f % .1f]dps\n",
                (unsigned long)n, a, ar[0], ar[1], ar[2], ax, ay, az,
                g, gr[0], gr[1], gr[2], gx, gy, gz);
        }
    }
    delay(2);
}
