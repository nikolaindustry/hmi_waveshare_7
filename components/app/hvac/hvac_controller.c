#include "hvac_controller.h"
#include "hvac_config.h"
#include "ac_state.h"
#include "bmp280.h"
#include "modbus_task.h"
#include "hyperwisor_app.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "hvac";

/* Latest snapshot of what the controller is doing. Updated at the
 * end of every tick under a critical section so the UI can read a
 * consistent copy without races. */
static hvac_status_t    s_status;
static portMUX_TYPE     s_status_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- internal enum for the fan step the controller *wants* ---- */
typedef enum {
    FAN_STEP_OFF  = 0,
    FAN_STEP_LOW  = 1,
    FAN_STEP_MED  = 2,
    FAN_STEP_HIGH = 3,
    FAN_STEP_MAX  = 4,
} fan_step_t;

/* ---- state tracked between ticks ---- */
static fan_step_t s_current_step   = FAN_STEP_OFF;
static bool       s_clutch_on      = false;

/* epoch = esp_timer_get_time() / 1000 (ms since boot). 0 = "never" */
static int64_t s_fan_on_since_ms   = 0;   /* when the *current* fan step turned on */
static int64_t s_clutch_off_since_ms = 0; /* when the clutch was last de-energised */
static int64_t s_master_off_since_ms = 0; /* when master power last went OFF */
static bool    s_poweroff_cooldown = false; /* blower-purge window after master OFF */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }


/* ---- helpers -------------------------------------------------------- */

/* Resolve a fan step to its coil number (or HVAC_COIL_UNUSED). */
static uint16_t step_to_coil(fan_step_t s)
{
    switch (s) {
        case FAN_STEP_LOW:  return HVAC_FAN_COIL_LOW;
        case FAN_STEP_MED:  return HVAC_FAN_COIL_MED;
        case FAN_STEP_HIGH: return HVAC_FAN_COIL_HIGH;
        case FAN_STEP_MAX:  return HVAC_FAN_COIL_MAX;
        case FAN_STEP_OFF:  /* fallthrough */
        default:            return HVAC_COIL_UNUSED;
    }
}

/* True if a step is actually wired (has a real coil behind it). */
static bool step_available(fan_step_t s)
{
    return step_to_coil(s) != HVAC_COIL_UNUSED;
}

/* If the requested step is not installed, degrade upward to the next
 * available one so the user's intent is still honoured as closely as
 * possible (e.g. MAX not wired -> HIGH). */
static fan_step_t normalise_step(fan_step_t s)
{
    if (s == FAN_STEP_OFF) return FAN_STEP_OFF;
    /* Walk down until we find an installed tap. */
    for (fan_step_t try = s; try > FAN_STEP_OFF; try--) {
        if (step_available(try)) return try;
    }
    return FAN_STEP_OFF;
}

/* Translate the UI's 1..5 fan slider into a physical step. */
static fan_step_t slider_to_step(int fan_level, bool power_on)
{
    if (!power_on) return FAN_STEP_OFF;
    if (fan_level <= 0) return FAN_STEP_OFF;
    if (fan_level <= 2) return FAN_STEP_LOW;
    if (fan_level == 3) return FAN_STEP_MED;
    if (fan_level == 4) return FAN_STEP_HIGH;
    return step_available(FAN_STEP_MAX) ? FAN_STEP_MAX : FAN_STEP_HIGH;
}

/* AUTO-mode decision: pick the fan step that best matches the delta. */
static fan_step_t auto_step_for_delta(float delta_c, fan_step_t previous)
{
    if (delta_c >= HVAC_TEMP_BAND_HIGH_C) {
        return step_available(FAN_STEP_MAX) ? FAN_STEP_MAX : FAN_STEP_HIGH;
    }
    if (delta_c >= HVAC_TEMP_BAND_MED_C)  return FAN_STEP_MED;
    if (delta_c >= HVAC_TEMP_BAND_LOW_C)  return FAN_STEP_LOW;
    if (delta_c <= -HVAC_TEMP_HYST_C) {
        /* At or below target -- keep LOW circulation so air still moves. */
        return FAN_STEP_LOW;
    }
    /* Inside the deadband -> hold whatever we already had. */
    return previous;
}

/* AUTO-mode clutch decision. Compressor only runs when we're above the
 * target with some margin; hysteresis prevents clutch chatter. */
static bool auto_clutch_for_delta(float delta_c, bool previous)
{
    if (delta_c >=  HVAC_TEMP_BAND_LOW_C)  return true;
    if (delta_c <= -HVAC_TEMP_HYST_C)      return false;
    return previous;
}


/* ---- Modbus output --------------------------------------------------
 *
 * Relay writes go through the shared modbus_task queue; the worker on
 * core 1 handles retries. We never block the control loop waiting for
 * confirmation -- if a write is lost the next tick will re-send exactly
 * the same state (idempotent). */

static void write_coil(uint16_t coil, bool on)
{
    if (coil == HVAC_COIL_UNUSED) return;
    modbus_task_request(coil, on);
}

/* Change fan step, enforcing break-before-make: old step OFF first,
 * then a short dead gap, then the new step ON. */
static void apply_fan_step(fan_step_t new_step)
{
    if (new_step == s_current_step) return;

    /* OFF old */
    uint16_t old_coil = step_to_coil(s_current_step);
    if (old_coil != HVAC_COIL_UNUSED) {
        write_coil(old_coil, false);
    }

    /* Dead time. We run at HVAC_TICK_MS cadence so the 80 ms gap is
     * well within a single tick -- just delay here and carry on. */
    if (old_coil != HVAC_COIL_UNUSED && new_step != FAN_STEP_OFF) {
        vTaskDelay(pdMS_TO_TICKS(HVAC_SPEED_GAP_MS));
    }

    /* ON new */
    uint16_t new_coil = step_to_coil(new_step);
    if (new_coil != HVAC_COIL_UNUSED) {
        write_coil(new_coil, true);
    }

    /* Bookkeeping. s_fan_on_since_ms tracks the moment the current
     * tap came up so clutch-prespin can wait for it. */
    if (new_step == FAN_STEP_OFF) {
        s_fan_on_since_ms = 0;
    } else {
        s_fan_on_since_ms = now_ms();
    }
    s_current_step = new_step;
    ESP_LOGI(TAG, "fan step -> %d", (int)new_step);
}

static void apply_clutch(bool on)
{
    if (on == s_clutch_on) return;
    write_coil(HVAC_CLUTCH_COIL, on);
    s_clutch_on = on;
    if (!on) s_clutch_off_since_ms = now_ms();
    ESP_LOGI(TAG, "clutch -> %s", on ? "ON" : "OFF");
}

/* Publish the current controller view for the UI. Called at the end
 * of every tick; the values reflect what we just commanded. */
static void publish_status(hvac_mode_t mode,
                           hvac_wait_t wait,
                           int         wait_ms_left,
                           bool        cabin_valid,
                           float       cabin_c,
                           float       target_c)
{
    hvac_status_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.mode         = mode;
    snap.wait         = wait;
    snap.wait_ms_left = wait_ms_left;
    snap.fan_step     = (int)s_current_step;
    uint16_t fc = step_to_coil(s_current_step);
    snap.fan_coil     = (fc == HVAC_COIL_UNUSED) ? -1 : (int)fc;
    snap.clutch_on    = s_clutch_on;
    snap.clutch_coil  = (HVAC_CLUTCH_COIL == HVAC_COIL_UNUSED) ? -1 : (int)HVAC_CLUTCH_COIL;
    snap.cabin_valid  = cabin_valid;
    snap.cabin_c      = cabin_c;
    snap.target_c     = target_c;
    snap.delta_c      = cabin_valid ? (cabin_c - target_c) : 0.0f;

    portENTER_CRITICAL(&s_status_mux);
    s_status = snap;
    portEXIT_CRITICAL(&s_status_mux);

    /* Voluntary cloud push every 2 ticks (~2s at 1 Hz tick). Internal
     * to hyperwisor_app: no-op when websocket not connected or cloud
     * bindings not configured. Rate-limited so the UART log stays clean
     * and the WS server doesn't see a 1 Hz firehose from each device. */
    static int s_push_counter = 0;
    if (++s_push_counter >= 2) {
        s_push_counter = 0;
        hyperwisor_app_push_hvac_status();
    }
}

void hvac_controller_get_status(hvac_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}


/* ---- the tick ------------------------------------------------------- */

static void hvac_tick(void)
{
    ac_state_t *st = ac_state();

    /* status accumulators for publish_status() at the bottom */
    hvac_mode_t  pub_mode        = HVAC_MODE_OFF;
    hvac_wait_t  pub_wait        = HVAC_WAIT_NONE;
    int          pub_wait_ms     = 0;
    bool         pub_cabin_valid = false;
    float        pub_cabin_c     = 0.0f;
    float        pub_target_c    = (float)st->temp_c;

    /* ------ master OFF handling ------ */
    if (!st->power) {
        /* Drop clutch immediately on the transition. */
        if (s_clutch_on) {
            apply_clutch(false);
        }
        if (!s_poweroff_cooldown && s_current_step != FAN_STEP_OFF) {
            s_poweroff_cooldown   = true;
            s_master_off_since_ms = now_ms();
            ESP_LOGI(TAG, "master OFF -- blower purge for %u ms",
                     (unsigned)HVAC_POWEROFF_BLOWER_MS);
        }
        if (s_poweroff_cooldown) {
            int64_t elapsed = now_ms() - s_master_off_since_ms;
            if (elapsed >= (int64_t)HVAC_POWEROFF_BLOWER_MS) {
                apply_fan_step(FAN_STEP_OFF);
                s_poweroff_cooldown = false;
                pub_mode = HVAC_MODE_OFF;
                pub_wait = HVAC_WAIT_NONE;
            } else {
                pub_mode = HVAC_MODE_PURGE;
                pub_wait = HVAC_WAIT_PURGE;
                pub_wait_ms = (int)((int64_t)HVAC_POWEROFF_BLOWER_MS - elapsed);
            }
        } else {
            /* Already idle, just make sure the fan is off. */
            if (s_current_step != FAN_STEP_OFF) apply_fan_step(FAN_STEP_OFF);
            pub_mode = HVAC_MODE_OFF;
            pub_wait = HVAC_WAIT_NONE;
        }
        publish_status(pub_mode, pub_wait, pub_wait_ms,
                       pub_cabin_valid, pub_cabin_c, pub_target_c);
        return;
    }

    /* Master came back ON -- drop any purge state. */
    s_poweroff_cooldown = false;

    /* ------ decide desired fan + clutch ------ */
    fan_step_t want_fan    = FAN_STEP_OFF;
    bool       want_clutch = false;

    if (st->auto_mode) {
        pub_mode = HVAC_MODE_AUTO;
        /* Need a valid cabin reading to run auto. */
        float cabin = 0.0f;
        if (!bmp280_get(&cabin, NULL)) {
            ESP_LOGW(TAG, "AUTO: no BMP280 reading, holding state");
            publish_status(HVAC_MODE_AUTO, HVAC_WAIT_NO_SENSOR, 0,
                           false, 0.0f, pub_target_c);
            return;  /* hold whatever we had */
        }
        pub_cabin_valid = true;
        pub_cabin_c     = cabin;
        float target = (float)st->temp_c;
        float delta  = cabin - target;

        want_fan    = auto_step_for_delta(delta,   s_current_step);
        want_clutch = auto_clutch_for_delta(delta, s_clutch_on);

        ESP_LOGD(TAG, "AUTO cabin=%.1f target=%.0f delta=%+.2f -> fan=%d clutch=%d",
                 cabin, target, delta, (int)want_fan, (int)want_clutch);
    } else {
        pub_mode = HVAC_MODE_MANUAL;
        /* Still expose cabin reading if available (for display only). */
        float cabin = 0.0f;
        if (bmp280_get(&cabin, NULL)) {
            pub_cabin_valid = true;
            pub_cabin_c     = cabin;
        }
        /* MANUAL: user's fan slider drives the step. The clutch follows
         * the fan -- if any fan step is selected AND the master is ON,
         * we try to engage cooling (still gated by the interlocks below).
         * Users who want fan-only behaviour can set fan_level to 0 in
         * the UI layer, or we can add an explicit clutch switch later. */
        want_fan    = slider_to_step(st->fan_level, true);
        want_clutch = (want_fan != FAN_STEP_OFF);
    }

    /* Normalise against installed hardware (e.g. MAX requested but not wired). */
    want_fan = normalise_step(want_fan);

    /* ------ apply fan first ------ */
    if (want_fan != s_current_step) {
        apply_fan_step(want_fan);
    }

    /* ------ apply clutch with interlocks ------ */
    if (want_clutch) {
        /* Gate 1: fan must be running. */
        if (s_current_step == FAN_STEP_OFF) {
            if (s_clutch_on) apply_clutch(false);
            pub_wait    = HVAC_WAIT_FAN_OFF;
            pub_wait_ms = 0;
            publish_status(pub_mode, pub_wait, pub_wait_ms,
                           pub_cabin_valid, pub_cabin_c, pub_target_c);
            return;
        }
        /* Gate 2: pre-spin time elapsed. */
        int64_t running = now_ms() - s_fan_on_since_ms;
        if (running < (int64_t)HVAC_BLOWER_PRESPIN_MS) {
            /* Still waiting; try again on the next tick. */
            if (s_clutch_on) apply_clutch(false);
            pub_wait    = HVAC_WAIT_PRESPIN;
            pub_wait_ms = (int)((int64_t)HVAC_BLOWER_PRESPIN_MS - running);
            publish_status(pub_mode, pub_wait, pub_wait_ms,
                           pub_cabin_valid, pub_cabin_c, pub_target_c);
            return;
        }
        /* Gate 3: anti-short-cycle. */
        if (!s_clutch_on && s_clutch_off_since_ms > 0) {
            int64_t rest = now_ms() - s_clutch_off_since_ms;
            if (rest < (int64_t)HVAC_CLUTCH_MIN_OFF_MS) {
                /* Respect the compressor's cooldown. */
                pub_wait    = HVAC_WAIT_LOCKOUT;
                pub_wait_ms = (int)((int64_t)HVAC_CLUTCH_MIN_OFF_MS - rest);
                publish_status(pub_mode, pub_wait, pub_wait_ms,
                               pub_cabin_valid, pub_cabin_c, pub_target_c);
                return;
            }
        }
        apply_clutch(true);
    } else {
        if (s_clutch_on) apply_clutch(false);
    }

    publish_status(pub_mode, pub_wait, pub_wait_ms,
                   pub_cabin_valid, pub_cabin_c, pub_target_c);
}


/* ---- task ---------------------------------------------------------- */

static void hvac_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "HVAC controller started (tick=%u ms)", (unsigned)HVAC_TICK_MS);
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        hvac_tick();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(HVAC_TICK_MS));
    }
}

esp_err_t hvac_controller_start(void)
{
    static bool started = false;
    if (started) return ESP_OK;

    BaseType_t ok = xTaskCreatePinnedToCore(
        hvac_task, "hvac",
        4096, NULL,
        4,          /* priority: above idle, below modbus worker */
        NULL,
        1);         /* pin to core 1 same as modbus worker */
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }
    started = true;
    return ESP_OK;
}
