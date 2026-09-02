#pragma once
// Pin map for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch devkit.
// Sources: Waveshare wiki/docs + the MaximeSahuc Arduino_GFX PlatformIO
// template. Do not edit unless the hardware changes.

// --- Power management (must be brought up first: it gates the AMOLED and
//     audio power rails) -----------------------------------------------------
#define XPOWERS_CHIP_AXP2101

// --- AMOLED display: CO5300 over QSPI ------------------------------------------
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_TE 13
#define LCD_WIDTH 410
#define LCD_HEIGHT 502
// CO5300 column start offset for this panel (from the template).
#define LCD_COL_OFFSET1 22

// --- Shared I2C bus: touch + IMU + RTC + PMU + audio codec -------------------
#define IIC_SDA 15
#define IIC_SCL 14

// --- Touch: FT3168 ---------------------------------------------------------------
#define TP_INT 38
#define TP_RESET 9

// --- IMU: QMI8658C (6-axis) -------------------------------------------------------
#define IMU_INT 21
// QMI8658 7-bit I2C address: 0x6B with SA0 high (Waveshare wiring), else 0x6A.
#define IMU_I2C_ADDR 0x6B

// --- RTC: PCF85063 -------------------------------------------------------------
#define RTC_INT 39

// --- Audio: ES8311 codec (I2S) + NS4150B speaker amp ------------------------
#define I2S_MCLK 16
#define I2S_DSDIN 40   // codec data in  (ESP -> codec, playback)
#define I2S_SCLK 41    // bit clock
#define I2S_ASDOUT 42  // codec/mic data out (codec -> ESP, capture)
#define I2S_LRCK 45
#define PA_CTRL 46     // NS4150B enable (active high)
#define ES8311_I2C_ADDR 0x18

// --- Vibration motor -------------------------------------------------------------
// The MaximeSahuc template defines a haptic motor here; the Waveshare spec
// page does not list one. Verify against the physical board / schematic
// before relying on it (see plan M0). If absent, feedback is audio-only.
#define HAPTIC_MOTOR_PIN 18

// --- microSD (SPI) -------------------------------------------------------------
#define SD_MOSI 1
#define SD_SCK 2
#define SD_MISO 3
#define SD_CS 17

// --- Buttons -------------------------------------------------------------------
#define BTN_BOOT 0
#define PWR_STATE 10
