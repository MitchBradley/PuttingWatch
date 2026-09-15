#pragma once
// Pin map for the Waveshare AMOLED watch devkits this project targets. Do not
// edit unless the hardware changes; see the wiki/schematic sources per board
// below.
//
// Two boards share this header, selected automatically by the compiler
// target (set by platformio.ini's `board =`):
//   ESP32-S3-Touch-AMOLED-2.06 (original) - dual-core Xtensa, 8 MB OPI
//     PSRAM, 32 MB flash. Its QMI8658 IMU died (see imu_probe.cpp); this
//     board is otherwise shelved, not removed.
//   ESP32-C6-Touch-AMOLED-2.06 (replacement) - single-core RISC-V, no
//     PSRAM, 16 MB flash, 160 MHz max. Same CO5300/FT3168/QMI8658/ES8311
//     parts, entirely different GPIOs. Confirmed from Waveshare's own
//     BSP component (waveshareteam/Waveshare-ESP32-components,
//     bsp/esp32_c6_touch_amoled_2_06/include/bsp/esp32_c6_touch_amoled_2_06.h)
//     and its pinout diagram, cross-checked against imu_probe.cpp's on-device
//     I2C scan / WHO_AM_I read.

#include <sdkconfig.h>

// --- Power management (must be brought up first: it gates the AMOLED and
//     audio power rails) -----------------------------------------------------
#define XPOWERS_CHIP_AXP2101

#if CONFIG_IDF_TARGET_ESP32S3
// ===========================================================================
// Waveshare ESP32-S3-Touch-AMOLED-2.06
// Sources: Waveshare wiki/docs + the MaximeSahuc Arduino_GFX PlatformIO
// template.
// ===========================================================================

// CPU: Xtensa dual-core, up to 240 MHz.
#define CPU_MHZ_FULL 240
#define CPU_MHZ_RUN  80

// AMOLED display: CO5300 over QSPI.
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
// CO5300 column start offset for this panel (from the template).
#define LCD_COL_OFFSET1 22

// Shared I2C bus: touch + IMU + RTC + PMU + audio codec.
#define IIC_SDA 15
#define IIC_SCL 14

// Touch: FT3168.
#define TP_INT 38
#define TP_RESET 9

// IMU: QMI8658C (6-axis). 7-bit I2C address: 0x6B with SA0 high (Waveshare
// wiring), else 0x6A.
#define IMU_I2C_ADDR 0x6B

// Audio: ES8311 codec (I2S) + NS4150B speaker amp.
#define I2S_MCLK 16
#define I2S_DSDIN 40   // codec data in  (ESP -> codec, playback)
#define I2S_SCLK 41    // bit clock
#define I2S_ASDOUT 42  // codec/mic data out (codec -> ESP, capture)
#define I2S_LRCK 45
#define PA_CTRL 46     // NS4150B enable (active high)

// Buttons.
#define BTN_BOOT 0

#elif CONFIG_IDF_TARGET_ESP32C6
// ===========================================================================
// Waveshare ESP32-C6-Touch-AMOLED-2.06 (replacement board)
// Source: waveshareteam/Waveshare-ESP32-components,
// bsp/esp32_c6_touch_amoled_2_06/include/bsp/esp32_c6_touch_amoled_2_06.h,
// and the product wiki's pinout diagram (both agree). No microSD slot, no
// haptic motor on this board (TF/vibration rows in Waveshare's own
// comparison table are both blank for this model) - if a coin motor gets
// wired up later, pick any free GPIO and add a define here.

// CPU: RISC-V single-core, up to 160 MHz.
#define CPU_MHZ_FULL 160
#define CPU_MHZ_RUN  80

// AMOLED display: CO5300 over QSPI.
#define LCD_SDIO0 1
#define LCD_SDIO1 2
#define LCD_SDIO2 3
#define LCD_SDIO3 4
#define LCD_SCLK 0
#define LCD_CS 5
#define LCD_RESET 11
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
// Same panel gap as the S3 board (esp_lcd_panel_set_gap(0x16, 0) in
// Waveshare's BSP == 22, 0).
#define LCD_COL_OFFSET1 22

// Shared I2C bus: touch + IMU + RTC + PMU + audio codec.
#define IIC_SDA 8
#define IIC_SCL 7

// Touch: FT3168.
#define TP_INT 15
#define TP_RESET 10

// IMU: QMI8658 (6-axis), same shared I2C bus. Address confirmed live via
// imu_probe.cpp (WHO_AM_I = 0x05 at 0x6B).
#define IMU_I2C_ADDR 0x6B

// Audio: ES8311 codec + ES7210 mic ADC (I2S) + speaker amp.
#define I2S_MCLK 19
#define I2S_DSDIN 23   // codec data in  (ESP -> codec, playback)
#define I2S_SCLK 20    // bit clock
#define I2S_ASDOUT 21  // codec/mic data out (codec -> ESP, capture)
#define I2S_LRCK 22
#define PA_CTRL 6      // amp enable (active high)

// Buttons. BOOT doubles as the C6's strapping pin, same role as GPIO0 on
// the S3 board, and (see main.cpp's checkBootShutdownButton) as the
// awake-shutdown trigger. PWR (Key3, schematic) pulls the AXP2101's PWRON
// pin low through a 510R resistor - a hardware power button, see
// Battery::shutdown() / main.cpp's enterPmuShutdown() - and wakes the board
// from the PMU-shutdown state entirely in hardware; nothing here reads it.
// It also drives a MOSFET gate that should, while awake, make GPIO18
// readable (idle LOW, HIGH while PWR is held), but that never showed a
// level change on-device, so it's unused - BOOT covers the awake-shutdown
// trigger instead.
#define BTN_BOOT 9
#define PWR_BTN 18

#else
#error "pin_config.h: unrecognised target - add a pin map for this board"
#endif
