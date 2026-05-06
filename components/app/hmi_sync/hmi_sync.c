#include "hmi_sync.h"
#include "ctrl_state.h"
#include "ac_state.h"
#include "hvac_controller.h"
#include "bmp280.h"
#include "modbus_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "hmi_sync";

/* Mirror block layout -- offsets relative to HMI_MIRROR_BASE (i.e.
 * index 0..63 into the 64-register block). Keep this table in sync
 * between encode_mirror() and write_mirror(). */
#define M_LINK_SEQ        0x00
#define M_RELAY_BITS      0x01
#define M_RELAY_PENDING   0x02

#define M_RGB_ROOF        0x10   /* 4 regs: on, hue, sat, brightness */
#define M_RGB_FLOOR       0x14   /* 4 regs: on, hue, sat, brightness */
#define M_STAR_ROOF       0x18   /* 2 regs: on, intensity            */

#define M_AC              0x20   /* 6 regs: power, auto, fan, temp, airflow, flags */

#define M_HVAC            0x30   /* 8 regs: mode, wait, wait_ms, fan_step, clutch_on,
                                            cabin_c*10, target_c*10, delta_c*10 */
#define M_CABIN_ENV       0x38   /* 2 regs: cabin_c*10, pressure_hpa (rounded u16) */

static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_mir_lock  = portMUX_INITIALIZER_UNLOCKED;

/* ---- SECONDARY ring state (also queried by primary's FC 0x03) ---- */
static uint16_t s_head_seq;                /* wraps; bumped per push         */
static uint16_t s_ring[HMI_INTENT_SLOT_COUNT][HMI_INTENT_REGS_PER_SLOT];
static uint8_t  s_ring_write_idx;          /* next slot to overwrite (LRU)   */

/* ---- SECONDARY mirror-receive state ---- */
static uint32_t s_mirror_seq;              /* bumped on each successful apply */
static int64_t  s_mirror_last_us = -1;     /* -1 = never seen                 */
static uint16_t s_local_link_seq;          /* copy of what primary sent       */

/* ---- PRIMARY ring-poll state ---- */
static uint16_t s_primary_last_seq;
static bool     s_secondary_online;

/* ================================================================
 *  SECONDARY helpers: intent ring producer / consumer
 * ================================================================ */

void hmi_sync_push_intent(hmi_cmd_t cmd, uint16_t a0, uint16_t a1, uint16_t a2)
{
    portENTER_CRITICAL(&s_ring_lock);
    uint8_t idx = s_ring_write_idx;
    s_ring[idx][0] = (uint16_t)cmd;
    s_ring[idx][1] = a0;
    s_ring[idx][2] = a1;
    s_ring[idx][3] = a2;
    s_ring_write_idx = (uint8_t)((idx + 1) % HMI_INTENT_SLOT_COUNT);
    s_head_seq++;
    uint16_t seq = s_head_seq;
    portEXIT_CRITICAL(&s_ring_lock);

    ESP_LOGI(TAG, "intent seq=%u cmd=%u a0=%u a1=%u a2=%u",
             (unsigned)seq, (unsigned)cmd,
             (unsigned)a0, (unsigned)a1, (unsigned)a2);
}

void hmi_sync_read_intent_block(uint16_t out[HMI_INTENT_RING_REGS])
{
    if (!out) return;
    portENTER_CRITICAL(&s_ring_lock);
    out[0] = s_head_seq;
    out[1] = HMI_INTENT_SLOT_COUNT;
    /* Flatten slots in canonical order: slot 0 is the OLDEST visible
     * entry given the current write index, slot HMI_INTENT_SLOT_COUNT-1
     * is the NEWEST. That way the primary can walk
     *   (head_seq - HMI_INTENT_SLOT_COUNT) .. head_seq-1
     * and index each new intent by subtracting from head_seq. */
    for (int k = 0; k < HMI_INTENT_SLOT_COUNT; k++) {
        int src = (s_ring_write_idx + k) % HMI_INTENT_SLOT_COUNT;
        int o   = 2 + k * HMI_INTENT_REGS_PER_SLOT;
        out[o + 0] = s_ring[src][0];
        out[o + 1] = s_ring[src][1];
        out[o + 2] = s_ring[src][2];
        out[o + 3] = s_ring[src][3];
    }
    portEXIT_CRITICAL(&s_ring_lock);
}

/* ================================================================
 *  Shared HSV -> RGB helper (needed when applying RGB intents on
 *  primary so the modbus_task_request_rgb call carries byte values)
 * ================================================================ */
static void hsv_to_rgb_u8(uint16_t h, uint8_t s, uint8_t v,
                          uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (h >= 360) h %= 360;
    if (s > 100)  s = 100;
    if (v > 100)  v = 100;

    uint32_t V = (uint32_t)v * 255u / 100u;
    if (s == 0) { *r = *g = *b = (uint8_t)V; return; }

    uint32_t S  = (uint32_t)s * 255u / 100u;
    uint32_t hi = h / 60;
    uint32_t f  = ((uint32_t)h - hi * 60) * 255u / 60u;
    uint32_t p  = V * (255u - S)                       / 255u;
    uint32_t q  = V * (255u - (S * f)          / 255u) / 255u;
    uint32_t t  = V * (255u - (S * (255u - f)) / 255u) / 255u;

    uint8_t R = 0, G = 0, B = 0;
    switch (hi) {
        case 0: R = V; G = t; B = p; break;
        case 1: R = q; G = V; B = p; break;
        case 2: R = p; G = V; B = t; break;
        case 3: R = p; G = q; B = V; break;
        case 4: R = t; G = p; B = V; break;
        default:R = V; G = p; B = q; break;
    }
    *r = R; *g = G; *b = B;
}

/* ================================================================
 *  PRIMARY: intent dispatch
 * ================================================================ */

/* Zone register map (must stay in sync with ui_tab_control.c). */
#define RGB_SLAVE_ADDR        0x20
#define RGB_START_REG_ROOF    0
#define RGB_START_REG_FLOOR   6

void hmi_sync_apply_intent(hmi_cmd_t cmd, uint16_t a0, uint16_t a1, uint16_t a2)
{
    switch (cmd) {
        case HMI_CMD_COIL_SET: {
            uint16_t coil = a0;
            bool     on   = a1 != 0;
            /* Primary's normal path: mark pending and let modbus_task
             * queue the FC 0x05 write; the worker will update ctrl_state
             * when the slave acknowledges. */
            modbus_task_request(coil, on);
            break;
        }
        case HMI_CMD_RGB_SET: {
            uint8_t zone = (uint8_t)a0;
            uint16_t hue = a1 % 360;
            uint8_t  sat = (uint8_t)((a2 >> 8) & 0x7F);
            uint8_t  brt = (uint8_t)(a2 & 0xFF);
            bool     on  = (a2 & 0x8000u) != 0;
            if (sat > 100) sat = 100;
            if (brt > 100) brt = 100;

            /* Update local zone state so the UI reflects the change. */
            ctrl_rgb_t *z = (zone == 1) ? ctrl_rgb_floor() : ctrl_rgb_roof();
            if (z) {
                z->hue = hue; z->sat = sat; z->brightness = brt; z->on = on;
            }

            /* Push to the slave. */
            uint8_t r = 0, g = 0, b = 0;
            hsv_to_rgb_u8(hue, sat, brt, &r, &g, &b);
            uint16_t start = (zone == 1) ? RGB_START_REG_FLOOR : RGB_START_REG_ROOF;
            modbus_task_request_rgb(RGB_SLAVE_ADDR, start, r, g, b, on);
            break;
        }
        case HMI_CMD_AC_SET: {
            ac_state_t *st = ac_state();
            if (!st) break;
            switch ((hmi_ac_field_t)a0) {
                case HMI_AC_FIELD_POWER:     st->power         = a1 != 0; break;
                case HMI_AC_FIELD_AUTO:      st->auto_mode     = a1 != 0; break;
                case HMI_AC_FIELD_FAN_LEVEL: st->fan_level     = (int)a1; break;
                case HMI_AC_FIELD_TEMP:      st->temp_c        = (int)a1; break;
                case HMI_AC_FIELD_AIRFLOW:   st->airflow_mask  = (int)a1; break;
                case HMI_AC_FIELD_FLAGS:
                    st->front_defrost = (a1 & 0x1) != 0;
                    st->rear_defrost  = (a1 & 0x2) != 0;
                    st->fresh_air     = (a1 & 0x4) != 0;
                    break;
                default: break;
            }
            break;
        }
        case HMI_CMD_STAR_SET: {
            ctrl_star_t *s = ctrl_star_roof();
            if (s) { s->on = a0 != 0; s->intensity = (uint8_t)(a1 > 100 ? 100 : a1); }
            break;
        }
        case HMI_CMD_NONE:
        default: break;
    }
}

uint16_t hmi_sync_primary_last_seq(void)          { return s_primary_last_seq; }
void     hmi_sync_primary_set_last_seq(uint16_t s){ s_primary_last_seq = s;    }
void     hmi_sync_primary_note_secondary(bool ok) { s_secondary_online = ok;   }
bool     hmi_sync_secondary_online(void)          { return s_secondary_online; }

/* ================================================================
 *  PRIMARY: encode mirror, SECONDARY: apply mirror
 * ================================================================ */

void hmi_sync_encode_mirror(uint16_t out[HMI_MIRROR_REGS])
{
    if (!out) return;
    memset(out, 0, HMI_MIRROR_REGS * sizeof(uint16_t));

    s_local_link_seq++;
    out[M_LINK_SEQ] = s_local_link_seq;

    /* Relay bits: walk both toggle arrays, pack into coil-indexed mask. */
    uint16_t relay = 0, pending = 0;
    int n = 0;
    ctrl_toggle_t *r = ctrl_reading_lights(&n);
    for (int i = 0; i < n; i++) {
        if (r[i].coil >= 16) continue;
        if (r[i].on)      relay   |= (1u << r[i].coil);
        if (r[i].pending) pending |= (1u << r[i].coil);
    }
    ctrl_toggle_t *sw = ctrl_switches(&n);
    for (int i = 0; i < n; i++) {
        if (sw[i].coil >= 16) continue;
        if (sw[i].on)      relay   |= (1u << sw[i].coil);
        if (sw[i].pending) pending |= (1u << sw[i].coil);
    }
    out[M_RELAY_BITS]    = relay;
    out[M_RELAY_PENDING] = pending;

    /* RGB zones. */
    ctrl_rgb_t *roof  = ctrl_rgb_roof();
    ctrl_rgb_t *floor = ctrl_rgb_floor();
    if (roof)  { out[M_RGB_ROOF+0]=roof->on?1:0;  out[M_RGB_ROOF+1]=roof->hue;
                 out[M_RGB_ROOF+2]=roof->sat;     out[M_RGB_ROOF+3]=roof->brightness; }
    if (floor) { out[M_RGB_FLOOR+0]=floor->on?1:0;out[M_RGB_FLOOR+1]=floor->hue;
                 out[M_RGB_FLOOR+2]=floor->sat;   out[M_RGB_FLOOR+3]=floor->brightness; }

    ctrl_star_t *st = ctrl_star_roof();
    if (st) { out[M_STAR_ROOF+0]=st->on?1:0; out[M_STAR_ROOF+1]=st->intensity; }

    ac_state_t *a = ac_state();
    if (a) {
        out[M_AC+0] = a->power ? 1 : 0;
        out[M_AC+1] = a->auto_mode ? 1 : 0;
        out[M_AC+2] = (uint16_t)a->fan_level;
        out[M_AC+3] = (uint16_t)a->temp_c;
        out[M_AC+4] = (uint16_t)a->airflow_mask;
        out[M_AC+5] = (uint16_t)((a->front_defrost ? 1 : 0) |
                                 (a->rear_defrost  ? 2 : 0) |
                                 (a->fresh_air     ? 4 : 0));
    }

    hvac_status_t h;
    hvac_controller_get_status(&h);
    out[M_HVAC+0] = (uint16_t)h.mode;
    out[M_HVAC+1] = (uint16_t)h.wait;
    out[M_HVAC+2] = (uint16_t)(h.wait_ms_left > 65535 ? 65535 : h.wait_ms_left);
    out[M_HVAC+3] = (uint16_t)h.fan_step;
    out[M_HVAC+4] = h.clutch_on ? 1 : 0;
    out[M_HVAC+5] = (int16_t)(h.cabin_c  * 10.0f);
    out[M_HVAC+6] = (int16_t)(h.target_c * 10.0f);
    out[M_HVAC+7] = (int16_t)(h.delta_c  * 10.0f);

    float tc = 0.0f, ph = 0.0f;
    if (bmp280_get(&tc, &ph)) {
        out[M_CABIN_ENV+0] = (int16_t)(tc * 10.0f);
        out[M_CABIN_ENV+1] = (uint16_t)(ph + 0.5f);
    }
}

void hmi_sync_write_mirror(const uint16_t in[HMI_MIRROR_REGS])
{
    if (!in) return;
    portENTER_CRITICAL(&s_mir_lock);
    s_mirror_seq++;
    s_mirror_last_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mir_lock);

    /* relay bits -> reading lights / switches on fields */
    uint16_t relay   = in[M_RELAY_BITS];
    uint16_t pending = in[M_RELAY_PENDING];
    int n = 0;
    ctrl_toggle_t *r = ctrl_reading_lights(&n);
    for (int i = 0; i < n; i++) {
        if (r[i].coil >= 16) continue;
        r[i].on      = ((relay   >> r[i].coil) & 1u) != 0;
        r[i].pending = ((pending >> r[i].coil) & 1u) != 0;
    }
    ctrl_toggle_t *sw = ctrl_switches(&n);
    for (int i = 0; i < n; i++) {
        if (sw[i].coil >= 16) continue;
        sw[i].on      = ((relay   >> sw[i].coil) & 1u) != 0;
        sw[i].pending = ((pending >> sw[i].coil) & 1u) != 0;
    }

    ctrl_rgb_t *roof = ctrl_rgb_roof();
    if (roof) {
        roof->on         = in[M_RGB_ROOF+0] != 0;
        roof->hue        = in[M_RGB_ROOF+1];
        roof->sat        = (uint8_t)(in[M_RGB_ROOF+2] > 100 ? 100 : in[M_RGB_ROOF+2]);
        roof->brightness = (uint8_t)(in[M_RGB_ROOF+3] > 100 ? 100 : in[M_RGB_ROOF+3]);
    }
    ctrl_rgb_t *floor = ctrl_rgb_floor();
    if (floor) {
        floor->on         = in[M_RGB_FLOOR+0] != 0;
        floor->hue        = in[M_RGB_FLOOR+1];
        floor->sat        = (uint8_t)(in[M_RGB_FLOOR+2] > 100 ? 100 : in[M_RGB_FLOOR+2]);
        floor->brightness = (uint8_t)(in[M_RGB_FLOOR+3] > 100 ? 100 : in[M_RGB_FLOOR+3]);
    }

    ctrl_star_t *st = ctrl_star_roof();
    if (st) {
        st->on        = in[M_STAR_ROOF+0] != 0;
        st->intensity = (uint8_t)(in[M_STAR_ROOF+1] > 100 ? 100 : in[M_STAR_ROOF+1]);
    }

    ac_state_t *a = ac_state();
    if (a) {
        a->power         = in[M_AC+0] != 0;
        a->auto_mode     = in[M_AC+1] != 0;
        a->fan_level     = (int)in[M_AC+2];
        a->temp_c        = (int)in[M_AC+3];
        a->airflow_mask  = (int)in[M_AC+4];
        uint16_t flags   = in[M_AC+5];
        a->front_defrost = (flags & 0x1) != 0;
        a->rear_defrost  = (flags & 0x2) != 0;
        a->fresh_air     = (flags & 0x4) != 0;
    }
    /* HVAC status + cabin env are read-only mirrors on secondary; the
     * climate tab on secondary reads ac_state/ctrl_rgb/ctrl_star but
     * does not expose hvac_controller_get_status or bmp280_get, so we
     * do not need to write-back to those local singletons here. */
}

bool hmi_sync_mirror_seen(void) { return s_mirror_last_us >= 0; }

uint32_t hmi_sync_mirror_seq(void) { return s_mirror_seq; }

uint32_t hmi_sync_mirror_age_ms(void)
{
    if (s_mirror_last_us < 0) return UINT32_MAX;
    int64_t delta = esp_timer_get_time() - s_mirror_last_us;
    if (delta < 0) return 0;
    return (uint32_t)(delta / 1000);
}
