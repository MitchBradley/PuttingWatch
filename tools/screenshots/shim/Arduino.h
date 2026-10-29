#pragma once
// Host stand-ins for the handful of Arduino/ESP symbols the UI sources touch,
// so src/ui.cpp and src/update_ui.cpp compile unchanged on a desktop. Only
// used by tools/screenshots; nothing here ships in the firmware.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#define LOW 0
#define HIGH 1
uint32_t millis();  // the harness's fake clock
inline void delay(uint32_t) {}
inline int digitalRead(int) { return HIGH; }
struct EspClass {
    void restart() {}
};
extern EspClass ESP;
