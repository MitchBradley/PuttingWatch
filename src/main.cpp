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
static Arduino_CO5300* amoled =
    new Arduino_CO5300(bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT,
                       LCD_COL_OFFSET1, 0, 0, 0);
static Arduino_GFX* gfx = amoled;  // generic drawing; `amoled` for setBrightness()

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
static volatile bool samplingPaused = false;    // true while asleep (or stopping)
static volatile bool lightSleepReq = false;     // set by a double background-tap
static volatile bool deepSleepReq = false;      // set by the Stop button
static uint32_t touchIgnoreUntilMs = 0;         // swallow the light-sleep wake tap

// AMOLED power. On the run (Monitoring) screen the panel is blanked (DISPOFF)
// while idle - you're watching the ball, not the watch. While blanked, loop()
// runs its lean blankedLoop() path: no LVGL, no rendering, just a wake-tap
// poll and the re-arm cue, with a longer delay so core 1 mostly idles. Any
// tap wakes it (full brightness for kBrightHoldMs); the wake tap is swallowed
// so it can't blind-press a control. Other screens stay on. A double
// background-tap on the run screen goes to light sleep (ui::onTap). All state
// here is touched from the loop thread only.
// Click sensitivity, shown to the user as a plain "level" 1-10 instead of a
// raw degree count - degrees (especially small ones, 3 and under) turned out
// to be hard for people to reason about. Level 1 = kLevelBaseDegrees, each
// step up adds kLevelStepDegrees. N is fixed for now; the plan is to make it
// user-configurable later (per-stroke / green-speed tuning), at which point
// this stops being a compile-time constant - see SwingTracker::Config's
// clickThresholdDegreesMin/Max, which must stay in sync with level 1 and 10.
static constexpr float kLevelBaseDegrees = 3.5f;  // level 1
static constexpr float kLevelStepDegrees = 1.2f;  // degrees per level (N)
static int levelForDegrees(float deg) {
    return (int)lroundf((deg - kLevelBaseDegrees) / kLevelStepDegrees) + 1;
}
static float degreesForLevel(int level) {
    return kLevelBaseDegrees + (float)(level - 1) * kLevelStepDegrees;
}

static constexpr uint8_t  kBrightFull     = 0xD0;  // CO5300 init value
static constexpr uint32_t kBrightHoldMs   = 5000;
static constexpr uint32_t kWakeSwallowMs  = 400;   // ignore touches this long after a wake
static constexpr uint32_t kDoubleTapMs    = 400;
static constexpr uint32_t kBlankedLoopMs  = 25;    // core-1 loop period while blanked
static uint32_t lastTapMs        = 0;   // any touch, set in touchRead() / blankedLoop()
static uint32_t lastBgTapMs      = 0;   // background clicks only, for double-tap
static bool     runScreenBlanked = false;
static bool     lastReady        = false;  // ready-cue edge, maintained in both loop modes

// A swing angle this big on the run screen isn't normal putting motion -
// probably someone looking at the watch mid-motion, or just checking it
// while holding the putter - so light it up regardless of whether a tap
// caused it. Independent hold from the tap-driven kBrightHoldMs (own
// timestamp, own duration): a stray big angle shouldn't cut a tap's hold
// short, and vice versa - the run screen only blanks once BOTH have expired.
// One-shot on the RISING edge only (angleWasOverThreshold tracks it): if the
// angle just stays past the threshold (e.g. the watch is left tilted), the
// hold must still expire on schedule rather than being continuously
// re-armed and never blanking at all.
static constexpr float    kAngleWakeThresholdDeg = 45.0f;
static constexpr uint32_t kAngleWakeHoldMs       = 10000;
static uint32_t lastAngleWakeMs      = 0;      // set on the rising edge past the threshold
static bool     angleWasOverThreshold = false;  // edge state, maintained in both loop modes

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
    lastTapMs = millis();  // hold the run screen on / re-arm the 5 s
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
// Double background-tap -> light sleep. Display off, amp off, both cores
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
    clicker.setAmpEnabled(false);

    esp_sleep_enable_gpio_wakeup();
    gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_BOOT, GPIO_INTR_LOW_LEVEL);
    esp_light_sleep_start();
    // ---------------- resumes here on wake ----------------
    gpio_wakeup_disable((gpio_num_t)TP_INT);
    gpio_wakeup_disable((gpio_num_t)BTN_BOOT);

    gfx->displayOn();
    amoled->setBrightness(kBrightFull);
    runScreenBlanked = false;  // displayOn() fully re-enabled the panel
    gfx->fillScreen(BLACK);
    // Amp stays off; the next loop() iteration re-enables it if we woke back
    // into the run state.
    lv_obj_invalidate(lv_screen_active());  // full repaint

    touch->IIC_Interrupt_Flag = false;
    touchIgnoreUntilMs = millis() + 400;  // don't let the wake tap hit a button
    lastTapMs = millis();                 // wake bright, then dim after the hold
    samplingPaused = false;
    USBSerial.println("woke from light sleep");
}

// ---------------------------------------------------------------------------
// Stop button -> deep sleep: everything off (~10 uA), RAM lost. Wakes only on
// the BOOT button (GPIO0) and comes back through a full reboot / setup(), so
// it lands on the "Tap when in putting position" screen. The FT3168 touch INT
// (GPIO38) is NOT an RTC pad and can't wake deep sleep, hence BOOT only.
//
// S3 board only. On the C6 board neither onboard button (BOOT=GPIO9,
// PWR=GPIO18) is in the RTC/LP-IO range (GPIO0-7), so ext1 deep-sleep wakeup
// has no button to use, and gpio_deep_sleep_hold_en()/_dis() (the whole-chip
// pad-hold call this function relies on) don't even exist for C6 - it only
// has the per-pin gpio_hold_en(), which already latches state through deep
// sleep on that target with no separate enable call. loop() routes the Stop
// button to enterPmuShutdown() on C6 instead of here - see that function.
#if !CONFIG_IDF_TARGET_ESP32C6
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
#endif  // !CONFIG_IDF_TARGET_ESP32C6

#if CONFIG_IDF_TARGET_ESP32C6
// Stop button -> AXP2101 PMU shutdown. Per the schematic, this board's PWR
// key (Key3) pulls the AXP2101's PWRON pin (30) low through a 510R resistor -
// a real hardware power button, not just another GPIO. XPowersLib's
// shutdown() writes the PMU's own shutdown bit, which cuts every rail but
// VRTC, the ESP32's 3V3 supply included: this is genuinely off (well below
// any ESP32-side sleep mode, which by definition keeps the chip powered),
// and it's the mechanism this board's hardware was actually built for -
// unlike deep sleep, which has no RTC/LP-IO wake pin available on this
// board (see enterDeepSleep()'s comment). Wakes purely in hardware: pressing
// PWR pulls PWRON low again, the PMU treats that as a power-on request, and
// the ESP32 does a full cold boot back to setup() - same "lands on
// WaitingForCalibration" behavior as the S3's deep sleep, just reached via
// the PMU instead of the SoC's own sleep path.
static void enterPmuShutdown() {
    USBSerial.println("stop: PMU shutdown (press PWR to wake)");
    samplingPaused = true;

    // The run screen may currently be blanked (bare DISPOFF, see
    // blankedLoop()). DISPOFF doesn't clear GRAM or stop QSPI writes, it
    // only stops the panel emitting light - so draw the new frame FIRST
    // (fillScreen, then the Bye screen) and only turn the panel back on
    // (displayOn(), which also sends SLPOUT - the bare DISPON
    // wakeRunScreen() uses wasn't reliable here) once GRAM already holds the
    // right content. Doing it the other way round flashes the stale
    // run-screen frame that was still sitting in GRAM.
    bool wasBlanked = runScreenBlanked;
    if (wasBlanked) {
        gfx->fillScreen(BLACK);
    }

    // Visible feedback that the press registered, since the PMU rail-cut
    // that follows gives no other sign anything happened. Forces its own
    // flush (lv_refr_now), so it's on screen even though loop()'s normal
    // ~20 Hz render won't run again before displayOff(). Safe to call
    // whether or not the panel is currently on.
    ui::showByeScreen();

    if (wasBlanked) {
        gfx->displayOn();
        amoled->setBrightness(kBrightFull);
        runScreenBlanked = false;
    }

    delay(600);

    USBSerial.flush();
    delay(20);
    gfx->displayOff();
    clicker.setAmpEnabled(false);

    if (battery.ok()) {
        battery.shutdown();  // rails drop here - nothing below this line runs
    }

    // Only reached if the PMU never initialised (see setup()'s "AXP2101
    // begin failed" case) - don't strand the user with a dead Stop button.
    USBSerial.println("PMU shutdown unavailable (AXP2101 not initialised) - falling back to light sleep");
    samplingPaused = false;
    enterLightSleep();
}

// BOOT button while awake: a press triggers the same graceful shutdown as
// tapping the on-screen Stop button (see enterPmuShutdown()). PWR (GPIO18)
// was the original plan - pulling PWRON low doubles as a MOSFET-inverted
// GPIO18 read per the schematic - but on-device that pin never showed a
// level change when pressed (verified by swapping this same poll to GPIO9,
// which worked immediately), so BOOT does the job instead. PWR still wakes
// the watch from the PMU-shutdown state via PWRON - that path is pure
// hardware and doesn't involve this code at all.
//
// Idle HIGH (INPUT_PULLUP, set in setup()), pulled LOW when pressed.
// Debounced by a minimum gap between triggers, not a hold timer - a
// mechanical button bouncing on one press is the only thing being filtered,
// and polling at loop()'s ~50 Hz (or blankedLoop()'s ~40 Hz) is already slow
// enough that a real bounce can't retrigger past it.
static bool bootBtnWasPressed = false;
static uint32_t bootBtnLastTriggerMs = 0;
static void checkBootShutdownButton() {
    bool pressed = digitalRead(BTN_BOOT) == LOW;
    if (pressed && !bootBtnWasPressed && millis() - bootBtnLastTriggerMs > 500) {
        bootBtnLastTriggerMs = millis();
        deepSleepReq = true;
        USBSerial.println("BOOT button press -> requesting shutdown");
    }
    bootBtnWasPressed = pressed;
}
#endif  // CONFIG_IDF_TARGET_ESP32C6

// ---------------------------------------------------------------------------
static void wireUiHooks() {
    // A tap that isn't on a button: start calibration from the idle screen;
    // on the run screen a double-tap drops into light sleep (a single tap just
    // wakes the dimmed screen, handled in touchRead).
    ui::onTap = [] {
        xSemaphoreTake(trackerMux, portMAX_DELAY);
        SwingTracker::State st = tracker.state();
        if (st == SwingTracker::State::WaitingForCalibration) tracker.tap();
        xSemaphoreGive(trackerMux);
        if (st == SwingTracker::State::Monitoring || st == SwingTracker::State::Done) {
            uint32_t now = millis();
            if (now - lastBgTapMs < kDoubleTapMs) lightSleepReq = true;
            lastBgTapMs = now;
        }
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
        int newLevel = 0;
        if (adjust) {
            int curLevel = levelForDegrees(tracker.clickThresholdDegrees());
            newLevel = curLevel + delta;
            if (newLevel < 1) newLevel = 1;
            if (newLevel > 10) newLevel = 10;
            tracker.setClickThresholdDegrees(degreesForLevel(newLevel));
        }
        xSemaphoreGive(trackerMux);
        if (adjust) clicker.playNumber(newLevel);  // "one".."ten"; tone fallback if no samples
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
#if !CONFIG_IDF_TARGET_ESP32C6
    gpio_deep_sleep_hold_dis();
#endif

    trackerMux = xSemaphoreCreateMutex();

    if (!gfx->begin()) USBSerial.println("gfx->begin() failed");
    gfx->fillScreen(BLACK);

    Wire.begin(IIC_SDA, IIC_SCL, 400000);

    // Also the wake source out of light sleep, and (C6) the awake-shutdown
    // trigger polled by checkBootShutdownButton().
    pinMode(BTN_BOOT, INPUT_PULLUP);

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

    // --- Model callbacks (run inside tracker.update on core 0, under the
    // tracker mutex). playClick() is just a non-blocking queue send to the
    // audio task, so fire it straight from here - the click stays tight to
    // the stroke regardless of what the core-1 loop() is doing. ---
    tracker.onClick = [] { clicker.playClick(); };
    // No calibration-complete cue here: the click interval is spoken from
    // loop() when the main (Monitoring) screen first appears - see below.

    xTaskCreatePinnedToCore(samplingTask, "sampling", 4096, nullptr, 2, nullptr, 0);

    USBSerial.printf("setup done (imu=%d)\n", imuOk);
}

// AXP2101 battery voltage -> EMA-smoothed mV / % (the chip's own fuel-gauge %
// is unusable on this board). Every 5 s, and once on the first call.
static void pollBattery() {
    static uint32_t last = 0;
    if (last && millis() - last < 5000) return;
    last = millis();
    int mv = battery.millivolts();
    if (mv <= 0) return;
    static int emaMv = 0;
    emaMv = (emaMv == 0) ? mv : emaMv + (mv - emaMv) / 3;
    batteryMv = emaMv;
    batteryPct = Battery::percentForMillivolts(emaMv);
}

// One diagnostic line per second.
static void serialLog() {
    static uint32_t last = 0;
    if (!USBSerial || millis() - last < 1000) return;
    last = millis();
    USBSerial.printf("state=%d phase=%d ready=%d angle=%.1f clicks=%d batt=%d%% %d mV heap=%u\n",
                     (int)tracker.state(), (int)tracker.swingPhase(), tracker.isReadyForSwing(),
                     tracker.swingAngleDegrees(), tracker.clickCount(), batteryPct, batteryMv,
                     (unsigned)ESP.getFreeHeap());
}

// Un-blank the run screen: DISPON, last frame is still in GRAM, force a
// repaint, and hand control back to the normal loop() path.
static void wakeRunScreen() {
    bus->sendCommand(CO5300_C_DISPON);
    amoled->setBrightness(kBrightFull);
    lv_obj_invalidate(lv_screen_active());
    runScreenBlanked = false;
}

// Lean loop path while the run screen is blanked: no LVGL, no rendering. The
// 100 Hz sampling and the click audio run on core 0 regardless; here we only
// poll for a wake tap and keep the re-arm cue going, then sleep kBlankedLoopMs
// so core 1 mostly sits in waiti.
static void blankedLoop() {
#if CONFIG_IDF_TARGET_ESP32C6
    checkBootShutdownButton();
#endif

    // Wake tap: any FT3168 touch past the swallow window lights the screen.
    // The touch is consumed here (swallow window) so it can't blind-press a
    // control; a deliberate follow-up tap does.
    if (touch->IIC_Interrupt_Flag && (int32_t)(millis() - touchIgnoreUntilMs) >= 0) {
        touch->IIC_Interrupt_Flag = false;
        lastTapMs = millis();
        touchIgnoreUntilMs = millis() + kWakeSwallowMs;
        wakeRunScreen();
    }

    if (xSemaphoreTake(trackerMux, pdMS_TO_TICKS(5)) == pdTRUE) {
        bool ready = tracker.isReadyForSwing();
        bool inRun = tracker.state() == SwingTracker::State::Monitoring;
        float angle = tracker.swingAngleDegrees();
        xSemaphoreGive(trackerMux);
        if (ready && !lastReady) clicker.playCue(CUE_READY);
        lastReady = ready;
        if (!inRun) wakeRunScreen();  // defensive: can't happen without a tap
        // Big swing angle while blanked wakes the screen too, not just a
        // tap - see kAngleWakeThresholdDeg / kAngleWakeHoldMs. Rising edge
        // only, so holding a big angle can't block the screen from ever
        // blanking again.
        bool angleOver = inRun && angle > kAngleWakeThresholdDeg;
        if (angleOver && !angleWasOverThreshold) {
            lastAngleWakeMs = millis();
            wakeRunScreen();
        }
        angleWasOverThreshold = angleOver;
    }

    pollBattery();
    serialLog();
    delay(kBlankedLoopMs);
}

void loop() {
#if CONFIG_IDF_TARGET_ESP32C6
    checkBootShutdownButton();
#endif

    if (deepSleepReq) {
#if CONFIG_IDF_TARGET_ESP32C6
        deepSleepReq = false;
        enterPmuShutdown();  // never returns unless the PMU never initialised
#else
        enterDeepSleep();  // never returns
#endif
    }
    if (lightSleepReq) {
        lightSleepReq = false;
        enterLightSleep();
    }

    if (runScreenBlanked) {
        blankedLoop();
        return;
    }

    lv_timer_handler();
    pollBattery();

    // Snapshot the Model at ~20 Hz for the view + the audio/power logic.
    static uint32_t lastRender = 0;
    if (millis() - lastRender >= 50) {
        lastRender = millis();
        ui::Snapshot snap;
        if (xSemaphoreTake(trackerMux, pdMS_TO_TICKS(5)) == pdTRUE) {
            snap.state = tracker.state();
            snap.phase = tracker.swingPhase();
            snap.ready = tracker.isReadyForSwing();
            snap.angleDeg = tracker.swingAngleDegrees();
            snap.clickLevel = levelForDegrees(tracker.clickThresholdDegrees());
            snap.clickCount = tracker.clickCount();
            tracker.restGravity().store(snap.restG);
            xSemaphoreGive(trackerMux);
            snap.imuOk = imu.ok();
            snap.batteryPct = batteryPct;
            snap.batteryMv = batteryMv;
            ui::render(snap);

            // Power: the NS4150B amp only needs to be on while sounds can play.
            // Enable it as soon as calibration starts (Accumulating) rather
            // than on entry to Monitoring, so it has the full ~2 s hold-still
            // window to leave shutdown - otherwise the attack of the first
            // cue on the main screen is clipped. Off again on Recal (back to
            // WaitingForCalibration).
            clicker.setAmpEnabled(snap.state != SwingTracker::State::WaitingForCalibration);

            // "Ready" cue: speak assets/ready.wav on a rising edge of the
            // armed state - covers both "just landed on the Monitoring
            // screen" (isReadyForSwing() is already true the instant
            // Monitoring begins, so this fires right there - no separate
            // click-level announcement) and "re-armed for the next stroke
            // after a putt". (lastReady is a file global - blankedLoop()
            // maintains it too.)
            if (snap.ready && !lastReady) {
                clicker.playCue(CUE_READY);
            }
            lastReady = snap.ready;

            // Power: run the CPU at 80 MHz in the run (Monitoring) state. The
            // workload there - 100 Hz sampling + gravity/swing math, 20 Hz
            // partial LVGL render, on-demand audio - doesn't need 240 MHz, and
            // 80 MHz keeps the same 80 MHz APB clock so I2C/I2S/timers are
            // unaffected. Full speed elsewhere (boot, the image-heavy waiting
            // screen, calibration).
            static uint32_t curCpuMhz = CPU_MHZ_FULL;
            uint32_t wantCpuMhz =
                (snap.state == SwingTracker::State::Monitoring) ? CPU_MHZ_RUN : CPU_MHZ_FULL;
            if (wantCpuMhz != curCpuMhz) {
                setCpuFrequencyMhz(wantCpuMhz);
                curCpuMhz = wantCpuMhz;
                USBSerial.printf("cpu -> %u MHz\n", (unsigned)getCpuFrequencyMhz());
            }

            bool runScreen = snap.state == SwingTracker::State::Monitoring;
            // Rising edge only - see the angleWasOverThreshold comment above.
            bool angleOver = runScreen && snap.angleDeg > kAngleWakeThresholdDeg;
            if (angleOver && !angleWasOverThreshold) lastAngleWakeMs = millis();
            angleWasOverThreshold = angleOver;

            // Power: on the run screen, once kBrightHoldMs has elapsed since
            // the last tap AND kAngleWakeHoldMs since the last big-angle
            // event, blank the panel (DISPOFF - bare command, ~0 OLED
            // emission, last frame kept in GRAM) and drop into blankedLoop()
            // next iteration. wakeRunScreen() brings it back on a tap.
            if (runScreen && millis() - lastTapMs >= kBrightHoldMs &&
                millis() - lastAngleWakeMs >= kAngleWakeHoldMs) {
                bus->sendCommand(CO5300_C_DISPOFF);
                runScreenBlanked = true;
            }
        }
    }

    serialLog();

    // Yield so the idle task on this core runs (feeds the task watchdog) -
    // lv_timer_handler never blocks on its own.
    delay(2);
}
