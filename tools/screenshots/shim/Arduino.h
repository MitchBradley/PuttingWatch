#pragma once
// Host stand-ins for the handful of Arduino/ESP symbols the UI sources touch,
// so src/ui.cpp and src/update_ui.cpp compile unchanged on a desktop. Only
// used by tools/screenshots; nothing here ships in the firmware.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include "Print.h"
#include "pgmspace.h"
#define OUTPUT 1
#define LOW 0
#define HIGH 1
uint32_t millis();  // the harness's fake clock
using std::min;
using std::max;
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}
typedef bool boolean;
typedef uint8_t byte;
inline int digitalRead(int) { return HIGH; }
struct EspClass {
    void restart() {}
};
extern EspClass ESP;
