// PuttingWatch - golf putting stroke trainer on the Waveshare
// ESP32-S3-Touch-AMOLED-2.06.
//
// Port of PuttingCoachWatch (the watchOS bring-up subset): tap to calibrate a
// rest orientation, then a "click" (haptic pulse) each time the backswing
// passes a new angular threshold; clicking stops on the reversal and re-arms
// once the putter settles back near address.
//
// Layers:
//   lib/PuttingCore  - SwingTracker (Model) + GravityFilter, pure C++, unit-tested
//   lib/Imu          - QMI8658 driver wrapper
//   lib/Clicker      - feedback renderer (GPIO18 pulse; ES8311 audio TODO)
//   src/ui.*         - LVGL view
//
// STATUS: pure logic (lib/PuttingCore) is host-tested; everything in this file
// is a first cut pending on-device bring-up (see plan M0-M4).

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "HWCDC.h"

#include "pin_config.h"
#include "GravityFilter.h"
#include "SwingTracker.h"
#include "MotionSample.h"
#include "Imu.h"
#include "Clicker.h"
#include "ui.h"

HWCDC USBSerial;

// ---------------------------------------------------------------------------
// Display: CO5300 AMOLED over QSPI (offsets from Waveshare's own example).
// ---------------------------------------------------------------------------
static Arduino_DataBus* bus =
    new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_GFX* gfx =
    new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT,
                       LCD_COL_OFFSET1, 0, 0, 0);

// ---------------------------------------------------------------------------
// Touch: FT3168 on the shared I2C bus (Waveshare Arduino_DriveBus wrapper).
// ---------------------------------------------------------------------------
static std::shared_ptr<Arduino_IIC_DriveBus> iicBus =
    std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
static void touchIsr();
static std::unique_ptr<Arduino_IIC> touch(
    new Arduino_FT3x68(iicBus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, touchIsr));
static void touchIsr() { touch->IIC_Interrupt_Flag = true; }

// ---------------------------------------------------------------------------
// App objects.
// ---------------------------------------------------------------------------
static Imu imu;
static GravityFilter gravityFilter;
static SwingTracker tracker;
static Clicker clicker;

static SemaphoreHandle_t trackerMux;        // guards all `tracker` access
static volatile bool clickPending = false;
static volatile bool calibratedCuePending = false;
static volatile bool samplingPaused = false;

// LVGL draw buffer (partial mode). 100 lines so the tallest dynamic element
// (the montserrat-48 angle label, a fixed 70 px box - see ui.cpp) always
// redraws in a SINGLE tile => one monotonic top-to-bottom QSPI push, no
// tearing. Prefer INTERNAL RAM (software render is much faster there than
// into PSRAM); fall back to PSRAM if that block isn't free.
static constexpr uint32_t kBufLines = 100;  // 410 * 100 * 2 = ~82 KB
static lv_color_t* drawBuf = nullptr;

// ---------------------------------------------------------------------------
// LVGL glue.
// ---------------------------------------------------------------------------
static uint32_t lvglTick() { return millis(); }

static void dispFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(px_map), w, h);
    lv_display_flush_ready(disp);
}

static void touchRead(lv_indev_t*, lv_indev_data_t* data) {
    // Called every lv_timer_handler pass - do zero I2C traffic unless the
    // touch IRQ actually fired.
    if (!touch->IIC_Interrupt_Flag) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    touch->IIC_Interrupt_Flag = false;
    data->point.x = touch->IIC_Read_Device_Value(
        touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    data->point.y = touch->IIC_Read_Device_Value(
        touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    data->state = LV_INDEV_STATE_PRESSED;
}

// ---------------------------------------------------------------------------
// Sampling task: fixed 100 Hz, pinned to core 0 (the Arduino loop + LVGL run
// on core 1). Owns the IMU -> GravityFilter -> SwingTracker pipeline; the UI
// only ever reads a snapshot of the result.
// ---------------------------------------------------------------------------
static void samplingTask(void*) {
    const TickType_t period = pdMS_TO_TICKS(10);
    TickType_t last = xTaskGetTickCount();
    uint32_t prevMs = millis();

    for (;;) {
        vTaskDelayUntil(&last, period);
        if (samplingPaused) {
            prevMs = millis();
            continue;
        }

        Vec3 accel_g, gyro_rps;
        if (!imu.read(accel_g, gyro_rps)) continue;

        uint32_t now = millis();
        float dt = (now - prevMs) * 0.001f;
        prevMs = now;
        if (!gravityFilter.initialised()) gravityFilter.reset(accel_g);
        gravityFilter.update(accel_g, gyro_rps, dt);

        MotionSample s;
        s.t_ms = now;
        gravityFilter.gravity().store(s.gravity);
        gyro_rps.store(s.rotationRate);
        accel_g.store(s.accel);

        if (xSemaphoreTake(trackerMux, portMAX_DELAY) == pdTRUE) {
            tracker.update(s);
            xSemaphoreGive(trackerMux);
        }
    }
}

// ---------------------------------------------------------------------------
static void wireUiHooks() {
    ui::onTap = [] {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        tracker.tap();
        xSemaphoreGive(trackerMux);
    };
    ui::onRecal = [] {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        tracker.recal();
        xSemaphoreGive(trackerMux);
        samplingPaused = false;
    };
    ui::onStop = [] { samplingPaused = true; };
    ui::onSetClickThreshold = [](float d) {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        tracker.setClickThresholdDegrees(d);
        xSemaphoreGive(trackerMux);
    };
}

void setup() {
    USBSerial.begin(115200);
    // CRITICAL: without this, HWCDC.write() blocks the calling task for up to
    // ~1 s per call whenever no serial monitor is draining the port - which
    // dragged the whole render loop down to ~1 Hz when running untethered.
    // 0 = never block; drop bytes if the TX buffer is full.
    USBSerial.setTxTimeoutMs(0);
    USBSerial.println("\nPuttingWatch booting");

    trackerMux = xSemaphoreCreateMutex();

    if (!gfx->begin()) USBSerial.println("gfx->begin() failed");
    gfx->fillScreen(BLACK);

    Wire.begin(IIC_SDA, IIC_SCL, 400000);

    if (!touch->begin()) USBSerial.println("FT3168 begin failed");
    bool imuOk = imu.begin(Wire, IMU_I2C_ADDR);
    if (!imuOk) USBSerial.println("QMI8658 begin failed - check wiring/address");

    // --- LVGL ---
    lv_init();
    lv_tick_set_cb(lvglTick);

    size_t bufBytes = LCD_WIDTH * kBufLines * sizeof(lv_color_t);
    drawBuf = static_cast<lv_color_t*>(
        heap_caps_malloc(bufBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!drawBuf)
        drawBuf = static_cast<lv_color_t*>(heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, dispFlush);
    lv_display_set_buffers(disp, drawBuf, nullptr, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchRead);

    ui::init();
    wireUiHooks();

    clicker.begin();

    // --- Model callbacks (run inside tracker.update, already under the mutex -
    // just raise a flag, do the I/O from loop()). ---
    tracker.onClick = [] { clickPending = true; };
    tracker.onCalibrationComplete = [] { calibratedCuePending = true; };

    xTaskCreatePinnedToCore(samplingTask, "sampling", 4096, nullptr, 2, nullptr, 0);

    USBSerial.printf("setup done (imu=%d)\n", imuOk);
}

void loop() {
    lv_timer_handler();

    if (clickPending) { clickPending = false; clicker.playClick(); }
    if (calibratedCuePending) { calibratedCuePending = false; clicker.playBeep(2); }
    clicker.tick();

    // Snapshot the Model at ~20 Hz for the view. Cheap now that ui::render
    // only pushes what changed (the audio/haptic path is full-rate via the
    // sampling task, independent of this).
    static uint32_t lastRender = 0;
    uint32_t now = millis();
    if (now - lastRender >= 50) {
        lastRender = now;
        ui::Snapshot snap;
        if (xSemaphoreTake(trackerMux, pdMS_TO_TICKS(5)) == pdTRUE) {
            snap.state = tracker.state();
            snap.phase = tracker.swingPhase();
            snap.ready = tracker.isReadyForSwing();
            snap.angleDeg = tracker.swingAngleDegrees();
            snap.clickThresholdDeg = tracker.clickThresholdDegrees();
            snap.clickCount = tracker.clickCount();
            tracker.restGravity().store(snap.restG);
            xSemaphoreGive(trackerMux);
            snap.imuOk = imu.ok();
            ui::render(snap);
        }
    }

    static uint32_t lastLog = 0;
    if (USBSerial && now - lastLog >= 1000) {
        lastLog = now;
        USBSerial.printf("state=%d phase=%d ready=%d angle=%.1f clicks=%d heap=%u\n",
                         (int)tracker.state(), (int)tracker.swingPhase(),
                         tracker.isReadyForSwing(), tracker.swingAngleDegrees(),
                         tracker.clickCount(), (unsigned)ESP.getFreeHeap());
    }

    // Yield so the idle task on this core runs (feeds the task watchdog) -
    // lv_timer_handler never blocks on its own.
    delay(2);
}
