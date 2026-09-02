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
//   lib/Clicker      - feedback renderer (ES8311 codec + speaker, own task)
//   src/ui.*         - LVGL view
//
// STATUS: pure logic (lib/PuttingCore) is host-tested; everything in this file
// is a first cut pending on-device bring-up (see plan M0-M4).

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "HWCDC.h"

#include "pin_config.h"
#include "app_config.h"
#include "GravityFilter.h"
#include "SwingTracker.h"
#include "MotionSample.h"
#include "Imu.h"
#include "Clicker.h"
#include "Battery.h"
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
// Pass TP_RESET so touch->begin() hardware-resets the FT3168. Matters on a
// deep-sleep wake: the panel stayed powered through the sleep, so without a
// reset the chip comes back mid-I2C-transaction and the first reads fail
// (ESP_ERR_INVALID_STATE) until it self-recovers.
static std::unique_ptr<Arduino_IIC> touch(
    new Arduino_FT3x68(iicBus, FT3168_DEVICE_ADDRESS, TP_RESET, TP_INT, touchIsr));
static void touchIsr() { touch->IIC_Interrupt_Flag = true; }

// ---------------------------------------------------------------------------
// App objects.
// ---------------------------------------------------------------------------
static Imu imu;
static GravityFilter gravityFilter;

// Default model config, except: unless the "Rest captured" debug screen is
// built in, collapse the post-calibration Done dwell to zero so calibration
// hands straight off to Monitoring (see include/app_config.h).
static SwingTracker::Config makeTrackerConfig() {
    SwingTracker::Config c;
#if !SHOW_REST_CAPTURED_SCREEN
    c.monitoringDelayMs = 0;
#endif
    return c;
}
static SwingTracker tracker(makeTrackerConfig());
static Clicker clicker;
static Battery battery;
static volatile int batteryPct = -1;  // updated from loop() every few seconds
static volatile int batteryMv = -1;

static SemaphoreHandle_t trackerMux;        // guards all `tracker` access
static volatile bool clickPending = false;
static volatile bool samplingPaused = false;    // true while asleep (or stopping)
static volatile bool lightSleepReq = false;     // set by a tap on empty screen area
static volatile bool deepSleepReq = false;      // set by the Stop button
static uint32_t touchIgnoreUntilMs = 0;         // swallow the light-sleep wake tap

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
    if (!touch->IIC_Interrupt_Flag || (int32_t)(millis() - touchIgnoreUntilMs) < 0) {
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
            last = xTaskGetTickCount();  // keep the delay baseline fresh across a sleep
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
// Tap on empty screen area -> light sleep. Display off, amp off, both cores
// halted; a screen touch (FT3168 INT low) or the BOOT button wakes it and it
// resumes exactly where it left off - RAM is retained through light sleep, so
// SwingTracker state survives.
static void enterLightSleep() {
    USBSerial.println("light sleep (touch screen to wake)");
    samplingPaused = true;

    // Wait for the Stop touch to lift and drain the FT3168 so its INT line
    // deasserts - a level-triggered wake armed while INT is already low
    // returns from sleep immediately.
    uint32_t t0 = millis();
    while (digitalRead(TP_INT) == LOW && millis() - t0 < 3000) {
        touch->IIC_Read_Device_Value(
            touch->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
        delay(25);
    }

    USBSerial.flush();
    delay(20);  // let the last serial bytes out before USB-CDC powers down
    gfx->displayOff();
    digitalWrite(PA_CTRL, LOW);

    esp_sleep_enable_gpio_wakeup();
    gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_BOOT, GPIO_INTR_LOW_LEVEL);
    esp_light_sleep_start();
    // ---------------- resumes here on wake ----------------
    gpio_wakeup_disable((gpio_num_t)TP_INT);
    gpio_wakeup_disable((gpio_num_t)BTN_BOOT);

    gfx->displayOn();
    gfx->fillScreen(BLACK);
    digitalWrite(PA_CTRL, HIGH);
    lv_obj_invalidate(lv_screen_active());  // full repaint

    touch->IIC_Interrupt_Flag = false;
    touchIgnoreUntilMs = millis() + 400;  // don't let the wake tap hit a button
    samplingPaused = false;
    USBSerial.println("woke from light sleep");
}

// ---------------------------------------------------------------------------
// Stop button -> deep sleep: everything off (~10 uA), RAM lost. Wakes only on
// the BOOT button (GPIO0) and comes back through a full reboot / setup(), so
// it lands on the "Tap when in putting position" screen. The FT3168 touch INT
// (GPIO38) is NOT an RTC pad and can't wake deep sleep, hence BOOT only.
static void enterDeepSleep() {
    USBSerial.println("stop: deep sleep (press BOOT to wake)");
    USBSerial.flush();
    delay(20);
    samplingPaused = true;
    gfx->displayOff();

    // Latch the amp-enable low through deep sleep (GPIO46 is not an RTC pad,
    // so without this it floats once the SoC powers down). S3 can hold
    // digital pads across deep sleep via gpio_deep_sleep_hold_en().
    digitalWrite(PA_CTRL, LOW);
    gpio_hold_en((gpio_num_t)PA_CTRL);
    gpio_deep_sleep_hold_en();

    rtc_gpio_pullup_en((gpio_num_t)BTN_BOOT);
    rtc_gpio_pulldown_dis((gpio_num_t)BTN_BOOT);
    esp_sleep_enable_ext1_wakeup(1ULL << BTN_BOOT, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();  // never returns
}

// ---------------------------------------------------------------------------
static void wireUiHooks() {
    // A tap that isn't on a button: start calibration from the idle screen,
    // otherwise drop into light sleep.
    ui::onTap = [] {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        SwingTracker::State st = tracker.state();
        if (st == SwingTracker::State::WaitingForCalibration) tracker.tap();
        xSemaphoreGive(trackerMux);
        if (st == SwingTracker::State::Monitoring || st == SwingTracker::State::Done)
            lightSleepReq = true;
    };
    ui::onRecal = [] {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        tracker.recal();
        xSemaphoreGive(trackerMux);
        samplingPaused = false;
    };
    ui::onStop = [] { deepSleepReq = true; };
    ui::onAdjustClickThreshold = [](int delta) {
        // The click interval only has meaning on the monitoring screen, and
        // its buttons are shown only there. Ignore a nudge in any other state
        // so a stray touch (e.g. a touch-controller phantom right after a
        // cold / deep-sleep boot, which was landing on "+") can't change the
        // value or speak it.
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        bool adjust = tracker.state() == SwingTracker::State::Monitoring;
        int nv = 0;
        if (adjust) {
            tracker.setClickThresholdDegrees(tracker.clickThresholdDegrees() + delta);
            nv = (int)lroundf(tracker.clickThresholdDegrees());  // clamped value
        }
        xSemaphoreGive(trackerMux);
        if (adjust) clicker.playNumber(nv);  // "four".."twelve"; tone fallback if no samples
    };
}

void setup() {
    USBSerial.begin(115200);
    // CRITICAL: without this, HWCDC.write() blocks the calling task for up to
    // ~1 s per call whenever no serial monitor is draining the port - which
    // dragged the whole render loop down to ~1 Hz when running untethered.
    // 0 = never block; drop bytes if the TX buffer is full.
    USBSerial.setTxTimeoutMs(0);
    // Diagnostic for the deep-sleep-drain question: reset reason 8 =
    // ESP_RST_DEEPSLEEP; wakeup cause 3 = ESP_SLEEP_WAKEUP_EXT1 (BOOT button).
    USBSerial.printf("\nPuttingWatch booting (reset=%d wake=%d)\n",
                     (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());

    // Release any pad holds left latched by a prior deep sleep (see
    // enterDeepSleep) before anything reconfigures those pins.
    gpio_hold_dis((gpio_num_t)PA_CTRL);
    gpio_deep_sleep_hold_dis();

    trackerMux = xSemaphoreCreateMutex();

    if (!gfx->begin()) USBSerial.println("gfx->begin() failed");
    gfx->fillScreen(BLACK);

    Wire.begin(IIC_SDA, IIC_SCL, 400000);

    pinMode(BTN_BOOT, INPUT_PULLUP);  // also a wake source out of light sleep

    if (!touch->begin()) USBSerial.println("FT3168 begin failed");
    // The hardware reset in touch->begin() can leave the FT3168's INT
    // asserted or a stale sample pending; swallow touches briefly so a
    // phantom press on boot (notably after a deep-sleep wake) can't land on
    // a button - this was hitting "+" and announcing "seven".
    touch->IIC_Interrupt_Flag = false;
    touchIgnoreUntilMs = millis() + 400;

    bool imuOk = imu.begin(Wire, IMU_I2C_ADDR);
    if (!imuOk) USBSerial.println("QMI8658 begin failed - check wiring/address");
    if (!battery.begin(Wire)) USBSerial.println("AXP2101 begin failed - no battery gauge");

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

    if (!clicker.begin()) USBSerial.println("Clicker/ES8311 init failed - audio disabled");

    // --- Model callbacks (run inside tracker.update, already under the mutex -
    // just raise a flag, do the I/O from loop()). ---
    tracker.onClick = [] { clickPending = true; };
    // No calibration-complete cue here: the click interval is spoken from
    // loop() when the main (Monitoring) screen first appears - see below.

    xTaskCreatePinnedToCore(samplingTask, "sampling", 4096, nullptr, 2, nullptr, 0);

    USBSerial.printf("setup done (imu=%d)\n", imuOk);
}

void loop() {
    if (deepSleepReq) enterDeepSleep();  // never returns
    if (lightSleepReq) {
        lightSleepReq = false;
        enterLightSleep();
    }

    lv_timer_handler();

    if (clickPending) { clickPending = false; clicker.playClick(); }
    clicker.tick();

    // Battery: poll the AXP2101 voltage every 5 s (and once on the first
    // loop), EMA-smooth it, and derive % from a LiPo voltage curve - the
    // chip's own fuel-gauge % is unusable on this board.
    static uint32_t lastBatt = 0;
    if (lastBatt == 0 || millis() - lastBatt >= 5000) {
        lastBatt = millis();
        int mv = battery.millivolts();
        if (mv > 0) {
            static int emaMv = 0;
            emaMv = (emaMv == 0) ? mv : emaMv + (mv - emaMv) / 3;
            batteryMv = emaMv;
            batteryPct = Battery::percentForMillivolts(emaMv);
        }
    }

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
            snap.batteryPct = batteryPct;
            snap.batteryMv = batteryMv;
            ui::render(snap);

            // Cue for "calibration done, main screen is up": speak the current
            // click interval (e.g. "seven") the first time we land on the
            // Monitoring screen after calibration. Replaces the old
            // double-beep, and holds the announcement back to this point
            // rather than firing it right after the session starts.
            static SwingTracker::State lastCueState = SwingTracker::State::WaitingForCalibration;
            bool enteredMonitoring = snap.state == SwingTracker::State::Monitoring &&
                                     lastCueState != SwingTracker::State::Monitoring;
            if (enteredMonitoring) {
                clicker.playNumber((int)lroundf(snap.clickThresholdDeg));
            }
            lastCueState = snap.state;

            // "Ready" cue: speak assets/ready.wav on a rising edge of the
            // armed state - i.e. when re-armed for the next stroke after a
            // putt. Skip the edge that coincides with entering Monitoring;
            // the click-interval announcement above already signals "go".
            static bool lastReady = false;
            if (snap.ready && !lastReady && !enteredMonitoring) {
                clicker.playCue(CUE_READY);
            }
            lastReady = snap.ready;

            // Power: run the CPU at 80 MHz in the run (Monitoring) state. The
            // workload there - 100 Hz sampling + gravity/swing math, 20 Hz
            // partial LVGL render, on-demand audio - doesn't need 240 MHz, and
            // 80 MHz keeps the same 80 MHz APB clock so I2C/I2S/timers are
            // unaffected. Full speed elsewhere (boot, the image-heavy waiting
            // screen, calibration).
            static uint32_t curCpuMhz = 240;
            uint32_t wantCpuMhz = (snap.state == SwingTracker::State::Monitoring) ? 80 : 240;
            if (wantCpuMhz != curCpuMhz) {
                setCpuFrequencyMhz(wantCpuMhz);
                curCpuMhz = wantCpuMhz;
                USBSerial.printf("cpu -> %u MHz\n", (unsigned)getCpuFrequencyMhz());
            }
        }
    }

    static uint32_t lastLog = 0;
    if (USBSerial && now - lastLog >= 1000) {
        lastLog = now;
        USBSerial.printf("state=%d phase=%d ready=%d angle=%.1f clicks=%d batt=%d%% %d mV heap=%u\n",
                         (int)tracker.state(), (int)tracker.swingPhase(),
                         tracker.isReadyForSwing(), tracker.swingAngleDegrees(),
                         tracker.clickCount(), batteryPct, batteryMv,
                         (unsigned)ESP.getFreeHeap());
    }

    // Yield so the idle task on this core runs (feeds the task watchdog) -
    // lv_timer_handler never blocks on its own.
    delay(2);
}
