#include "Clicker.h"
#include "pin_config.h"

void Clicker::begin() {
    pinMode(HAPTIC_MOTOR_PIN, OUTPUT);
    setMotor_(false);
}

void Clicker::setMotor_(bool on) {
    digitalWrite(HAPTIC_MOTOR_PIN, on ? HIGH : LOW);
    motorOn_ = on;
}

void Clicker::playClick() {
    if (pending_ < 8) pending_++;  // cap so a stuck caller can't run away
    if (!motorOn_ && edgeDueMs_ == 0) {
        setMotor_(true);
        edgeDueMs_ = millis() + kPulseOnMs;
        pending_--;
    }
}

void Clicker::playBeep(int n) {
    for (int i = 0; i < n; i++) playClick();
}

void Clicker::tick() {
    if (edgeDueMs_ == 0) return;
    if ((int32_t)(millis() - edgeDueMs_) < 0) return;

    if (motorOn_) {
        setMotor_(false);
        if (pending_ > 0) {
            edgeDueMs_ = millis() + kPulseGapMs;  // gap before the next pulse
        } else {
            edgeDueMs_ = 0;  // idle
        }
    } else {
        // gap elapsed - start the next queued pulse
        setMotor_(true);
        edgeDueMs_ = millis() + kPulseOnMs;
        pending_--;
    }
}
