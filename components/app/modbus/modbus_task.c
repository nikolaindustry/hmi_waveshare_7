#include "modbus_task.h"
#include "modbus_client.h"
#include "ctrl_state.h"
#include "hmi_sync.h"
#include "hmi_role.h"
#include "hyperwisor_app.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "bsp.h"     /* bsp_io_expander_set() for CH422G DE probe */

static const char *TAG = "modbus_task";

#define MB_SLAVE_ADDR       0x01
#define MB_POLL_INTERVAL_MS 500
#define MB_SEC_POLL_MS      100   /* how often primary reads secondary's intent ring */
#define MB_MIRROR_PUSH_MS   500   /* how often primary pushes mirror to secondary    */
#define MB_SEC_OFFLINE_THR  3     /* consecutive failures -> mark secondary offline  */
/* When the secondary HMI is unreachable, back off both its poll and its
 * mirror push to this slow rate. Otherwise we flood the bus with frames
 * addressed to a non-existent slave (FC03 every 100ms + FC10 137-byte
 * mirror every 500ms) which collides with the relay's reply window. */
#define MB_SEC_BACKOFF_MS   10000
#define MB_QUEUE_LEN        16
#define MB_WRITE_RETRIES    2
#define MB_OFFLINE_THRESH   3      /* consecutive poll failures before OFFLINE */

typedef struct {
    uint16_t coil;
    bool     on;
} mb_req_t;

/* Coalesced RGB slot per slave address.
 *
 * The UI calls modbus_task_request_rgb() potentially once per drag
 * event -- 60 Hz when the user whips the colour wheel. Sending each
 * call as its own Modbus frame at 9600 baud would saturate the bus
 * AND fall behind the user's finger. Instead we keep one "pending"
 * struct per slave and the worker consumes whatever's there on each
 * loop turn; in-between calls overwrite the slot so the slave always
 * gets the most recent colour and the queue depth is bounded. */
#define MB_RGB_SLOTS 4
typedef struct {
    bool     in_use;
    bool     dirty;      /* set by UI, cleared when worker pushes it */
    uint8_t  slave;
    uint16_t start_reg;  /* first register of the zone's 4-reg block */
    uint8_t  r, g, b;
    bool     on;
} mb_rgb_slot_t;

static mb_rgb_slot_t  s_rgb_slots[MB_RGB_SLOTS];
static portMUX_TYPE   s_rgb_lock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t  s_q;
static volatile bool  s_link_online  = false;
static volatile uint32_t s_seq       = 0;
static int            s_fail_streak  = 0;
static TaskHandle_t   s_task_handle  = NULL;

/* Pause/resume flags. s_pause_req is set by the caller (LVGL thread);
 * the worker loop picks it up at the start of each iteration and sets
 * s_paused_ack while sitting in a tight delay loop. When s_pause_req
 * drops back to false the worker exits the delay and resumes polling. */
static volatile bool  s_pause_req    = false;
static volatile bool  s_paused_ack   = false;

bool modbus_task_link_online(void)
{
    /* On the PRIMARY the worker task maintains s_link_online based on
     * relay poll/write success. On the SECONDARY the worker never runs --
     * the secondary is purely a Modbus slave (slave 0x10) answering to
     * the primary. Drive the link pill from "did we get a mirror push
     * from the primary recently?" instead: hmi_sync_mirror_seq() is
     * bumped on every successful FC10 write from the primary. */
    if (hmi_role_is_secondary()) {
        static uint32_t  last_seen_seq  = 0;
        static TickType_t last_seen_tick = 0;
        uint32_t  seq = hmi_sync_mirror_seq();
        TickType_t now = xTaskGetTickCount();
        if (seq != last_seen_seq) {
            last_seen_seq  = seq;
            last_seen_tick = now;
        }
        /* Primary pushes mirror every 500 ms; allow 2.5 s before flipping
         * the pill to offline so a single dropped push doesn't flicker it.
         * AND with the remote relay-link bit so the pill reflects end-to-end
         * reachability, not just "primary is alive." */
        if (last_seen_tick == 0) return false;   /* nothing ever received */
        bool mirror_fresh = (uint32_t)(now - last_seen_tick) < pdMS_TO_TICKS(2500);
        return mirror_fresh && hmi_sync_remote_relay_online();
    }
    return s_link_online;
}
uint32_t modbus_task_state_seq(void)
{
    /* Combine local state changes with received-mirror changes so the
     * UI repaint timer fires for both. Without this, the secondary's UI
     * was lagging by one tap: ctrl_state arrays were being updated by
     * hmi_sync_write_mirror() (which bumps mirror_seq) but s_seq never
     * moved, so the LVGL sync callback never noticed there was new state
     * to paint until the user's next local tap bumped s_seq again.
     * On the primary, mirror_seq stays 0 (mirror is encoded, never
     * applied locally), so this addition is a no-op there. */
    return s_seq + hmi_sync_mirror_seq();
}

esp_err_t modbus_task_pause(uint32_t timeout_ms)
{
    if (!s_task_handle) return ESP_ERR_INVALID_STATE;
    s_pause_req = true;
    /* Poll for the worker to reach the idle point. Timeout must allow
     * for the longest in-flight UART transaction (~500 ms worst case). */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (!s_paused_ack) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            s_pause_req = false;  /* bail out; don't leave worker stuck */
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

void modbus_task_resume(void)
{
    s_pause_req  = false;
    s_paused_ack = false;
}

/* Find the ctrl_toggle_t with a given coil number, if any. */
static ctrl_toggle_t *find_by_coil(uint16_t coil)
{
    int n = 0;
    ctrl_toggle_t *r = ctrl_reading_lights(&n);
    for (int i = 0; i < n; i++) if (r[i].coil == coil) return &r[i];
    ctrl_toggle_t *s = ctrl_switches(&n);
    for (int i = 0; i < n; i++) if (s[i].coil == coil) return &s[i];
    return NULL;
}

void modbus_task_request(uint16_t coil, bool on)
{
    /* Secondary HMI has no bus access: reroute coil taps as intents.
     * The primary will dispatch them through its own queue and the
     * confirmation flows back via the state mirror.
     *
     * Optimistic UI: paint the new state immediately so the tile flips
     * the instant the user taps, instead of waiting for the next mirror
     * push (~100-500 ms away). pending=true so the tile shows its
     * in-flight border until the mirror confirms (or corrects) it. */
    if (hmi_role_is_secondary()) {
        hmi_sync_push_intent(HMI_CMD_COIL_SET, coil, on ? 1 : 0, 0);
        ctrl_toggle_t *t = find_by_coil(coil);
        if (t) {
            t->on      = on;
            t->pending = true;
            s_seq++;
        }
        return;
    }

    if (!s_q) return;
    /* Mark pending immediately so the UI can repaint the "in flight" border
     * without waiting for the task. */
    ctrl_toggle_t *t = find_by_coil(coil);
    if (t) { t->pending = true; s_seq++; }

    mb_req_t req = { .coil = coil, .on = on };
    /* Non-blocking: if the queue is full, drop the oldest by overwriting.
     * That's safer than blocking the LVGL thread. */
    if (xQueueSend(s_q, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "request queue full, dropping coil=%u on=%d", coil, on);
        if (t) { t->pending = false; s_seq++; }
    }
}

void modbus_task_request_rgb(uint8_t slave, uint16_t start_reg,
                             uint8_t r, uint8_t g, uint8_t b,
                             bool on)
{
    /* Find-or-allocate the slot for this (slave, start_reg) pair so
     * Roof and Floor on the same slave don't stomp on each other. */
    portENTER_CRITICAL(&s_rgb_lock);
    int idx = -1;
    for (int i = 0; i < MB_RGB_SLOTS; i++) {
        if (s_rgb_slots[i].in_use &&
            s_rgb_slots[i].slave == slave &&
            s_rgb_slots[i].start_reg == start_reg) { idx = i; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < MB_RGB_SLOTS; i++) {
            if (!s_rgb_slots[i].in_use) {
                idx = i;
                s_rgb_slots[i].in_use    = true;
                s_rgb_slots[i].slave     = slave;
                s_rgb_slots[i].start_reg = start_reg;
                break;
            }
        }
    }
    if (idx >= 0) {
        s_rgb_slots[idx].r     = r;
        s_rgb_slots[idx].g     = g;
        s_rgb_slots[idx].b     = b;
        s_rgb_slots[idx].on    = on;
        s_rgb_slots[idx].dirty = true;
    }
    portEXIT_CRITICAL(&s_rgb_lock);

    /* Bump the state counter so open UI screens repaint. Without this a
     * colour change arriving from the wireless HMI, the wired secondary
     * or the cloud app would update ctrl_rgb_* silently and the RGB
     * screen would keep showing the old colour until it was rebuilt by
     * switching tabs. */
    s_seq++;
}

void modbus_task_bump_seq(void)
{
    s_seq++;
}

static void handle_write(const mb_req_t *req)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i <= MB_WRITE_RETRIES; i++) {
        err = modbus_client_write_coil(MB_SLAVE_ADDR, req->coil, req->on);
        if (err == ESP_OK) break;
    }
    ctrl_toggle_t *t = find_by_coil(req->coil);
    if (t) {
        t->pending = false;
        if (err == ESP_OK) {
            t->on = req->on;   /* confirmed */
        }
        s_seq++;
    }
    if (err == ESP_OK) {
        s_link_online  = true;
        s_fail_streak  = 0;
        /* Voluntary cloud push: user-relay coils 0..7 only. Coils 8..15
         * are HVAC-owned and are reported via hyperwisor_app_push_hvac_status(). */
        if (req->coil < 8) {
            hyperwisor_app_push_relay_state((int)req->coil, req->on);
        }
    } else {
        ESP_LOGW(TAG, "write coil %u = %d failed: %s",
                 req->coil, req->on, esp_err_to_name(err));
        if (++s_fail_streak >= MB_OFFLINE_THRESH) s_link_online = false;
    }
}

/* Scan every RGB slot; for each dirty one snapshot the current values
 * under the critical section, clear the dirty flag, then do the slow
 * Modbus write outside the section so the UI thread never blocks.
 *
 * Register layout (matches slave_rgb_arduino.ino):
 *   reg 0 = R, reg 1 = G, reg 2 = B, reg 3 = MODE
 *   (MODE_STATIC=0, MODE_OFF=2; we don't touch SPEED/BRIGHTNESS here)
 */
static void drain_rgb_slots(void)
{
    for (int i = 0; i < MB_RGB_SLOTS; i++) {
        uint8_t  slave = 0, r = 0, g = 0, b = 0;
        uint16_t start = 0;
        bool     on    = false;
        bool     dirty = false;

        portENTER_CRITICAL(&s_rgb_lock);
        if (s_rgb_slots[i].in_use && s_rgb_slots[i].dirty) {
            slave = s_rgb_slots[i].slave;
            start = s_rgb_slots[i].start_reg;
            r     = s_rgb_slots[i].r;
            g     = s_rgb_slots[i].g;
            b     = s_rgb_slots[i].b;
            on    = s_rgb_slots[i].on;
            s_rgb_slots[i].dirty = false;
            dirty = true;
        }
        portEXIT_CRITICAL(&s_rgb_lock);

        if (!dirty) continue;

        uint16_t regs[4] = {
            (uint16_t)r,
            (uint16_t)g,
            (uint16_t)b,
            on ? 0u /* MODE_STATIC */ : 2u /* MODE_OFF */,
        };
        ESP_LOGI(TAG, "RGB cmd: FC10 slave=0x%02x regs[%u..%u]=R%u G%u B%u MODE=%s",
                 slave, (unsigned)start, (unsigned)start + 3,
                 r, g, b, on ? "STATIC" : "OFF");
        esp_err_t err = modbus_client_write_hregs(slave, start, 4, regs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rgb write to 0x%02x reg %u failed: %s",
                     slave, (unsigned)start, esp_err_to_name(err));
        }
    }
}

/* FC 0x01 read coils 0..15 and reconcile against s_reading[]. */
static void poll_state(void)
{
    uint16_t mask = 0;
    esp_err_t err = modbus_client_read_coils(MB_SLAVE_ADDR, 0, 16, &mask);
    if (err != ESP_OK) {
        if (++s_fail_streak >= MB_OFFLINE_THRESH) s_link_online = false;
        return;
    }
    s_link_online = true;
    s_fail_streak = 0;

    bool changed = false;
    int n = 0;
    ctrl_toggle_t *r = ctrl_reading_lights(&n);
    for (int i = 0; i < n; i++) {
        if (r[i].coil == CTRL_COIL_NONE) continue;
        if (r[i].pending) continue;   /* in-flight writes own the state */
        bool want = ((mask >> r[i].coil) & 0x1) != 0;
        if (r[i].on != want) {
            r[i].on = want;
            changed = true;
            /* Push externally-observed relay changes (e.g. physical
             * switch on the relay board) to the cloud as well so the
             * dashboard reflects field state even when the HMI didn't
             * originate the change. */
            if (r[i].coil < 8) {
                hyperwisor_app_push_relay_state((int)r[i].coil, want);
            }
        }
    }
    if (changed) s_seq++;
}

/* ---- Dual-HMI: primary polls secondary's intent ring and pushes the
 * state mirror back. Both transactions reuse modbus_client_* helpers
 * which already take s_lock on the UART. Called only from the worker
 * loop (single-threaded), so no additional locking is required.
 * -------------------------------------------------------------------- */
static int s_sec_fail_streak = 0;

static void poll_secondary_intents(void)
{
    /* Step 1: read head_seq + ring_depth (2 regs) to see if anything new. */
    uint16_t head2[2] = {0};
    esp_err_t err = modbus_client_read_hregs(HMI_SECONDARY_SLAVE_ID,
                                             HMI_INTENT_HEAD_ADDR, 2, head2);
    if (err != ESP_OK) {
        if (++s_sec_fail_streak >= MB_SEC_OFFLINE_THR) {
            hmi_sync_primary_note_secondary(false);
        }
        return;
    }
    s_sec_fail_streak = 0;
    hmi_sync_primary_note_secondary(true);

    uint16_t head_seq = head2[0];
    uint16_t last_seq = hmi_sync_primary_last_seq();
    if (head_seq == last_seq) return;    /* nothing new */

    /* Step 2: slurp the whole slot block in one go (32 regs = 8 slots).
     * Simpler than windowing on wrap-around and still fits in one frame. */
    uint16_t slots[HMI_INTENT_SLOT_COUNT * HMI_INTENT_REGS_PER_SLOT];
    err = modbus_client_read_hregs(HMI_SECONDARY_SLAVE_ID,
                                   HMI_INTENT_SLOTS_BASE,
                                   HMI_INTENT_SLOT_COUNT * HMI_INTENT_REGS_PER_SLOT,
                                   slots);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sec slot read failed: %s", esp_err_to_name(err));
        return;
    }

    /* Walk new slots oldest->newest. hmi_sync_read_intent_block() on the
     * secondary side returns the ring flattened in that order. */
    uint16_t delta = (uint16_t)(head_seq - last_seq);
    if (delta > HMI_INTENT_SLOT_COUNT) delta = HMI_INTENT_SLOT_COUNT;
    uint16_t first = HMI_INTENT_SLOT_COUNT - delta;
    for (uint16_t i = first; i < HMI_INTENT_SLOT_COUNT; i++) {
        const uint16_t *s = &slots[i * HMI_INTENT_REGS_PER_SLOT];
        hmi_sync_apply_intent((hmi_cmd_t)s[0], s[1], s[2], s[3]);
    }
    hmi_sync_primary_set_last_seq(head_seq);
}

static void push_mirror_to_secondary(void)
{
    uint16_t mirror[HMI_MIRROR_REGS];
    hmi_sync_encode_mirror(mirror);
    esp_err_t err = modbus_client_write_hregs(HMI_SECONDARY_SLAVE_ID,
                                              HMI_MIRROR_BASE,
                                              HMI_MIRROR_REGS, mirror);
    if (err != ESP_OK) {
        if (++s_sec_fail_streak >= MB_SEC_OFFLINE_THR) {
            hmi_sync_primary_note_secondary(false);
        }
    } else {
        s_sec_fail_streak = 0;
        hmi_sync_primary_note_secondary(true);
    }
}

static void modbus_worker(void *arg)
{
    (void)arg;

    /* ---- RS-485 DE probe ----
     * Measured idle voltage (A=4V, B~0V, A-B=3.9V) proves the display's
     * SP3485EN is sitting in receive-only mode: DE is never asserted.
     * Unused CH422G bits 0, 6, 7 are our only candidates for a DE wire.
     * Try each one HIGH (and also all three together) and attempt slave 1
     * FC 0x01 read coils 0..1. The combination that gets ANY response
     * (ESP_OK or even a CRC-valid exception) identifies the DE bit. */
    ESP_LOGI(TAG, "---- RS-485 DE probe: trying CH422G unused bits ----");
    const uint8_t probe_bits[] = { 0, 6, 7 };
    const size_t  probe_n      = sizeof(probe_bits) / sizeof(probe_bits[0]);
    int           found_bit    = -1;

    /* Baseline (all unused bits low = current default shadow 0x18) */
    {
        uint16_t m = 0;
        esp_err_t e = modbus_client_read_coils(1, 0, 1, &m);
        ESP_LOGI(TAG, "probe baseline (no EXIO) slave=1 -> %s",
                 esp_err_to_name(e));
        if (e == ESP_OK) { found_bit = -2; goto probe_done; }
    }

    /* Single-bit probes */
    for (size_t i = 0; i < probe_n; i++) {
        uint8_t bit = probe_bits[i];
        bsp_io_expander_set(bit, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        uint16_t m = 0;
        esp_err_t e = modbus_client_read_coils(1, 0, 1, &m);
        ESP_LOGI(TAG, "probe EXIO%u=1 slave=1 -> %s",
                 (unsigned)bit, esp_err_to_name(e));
        if (e == ESP_OK) { found_bit = bit; goto probe_done; }
        bsp_io_expander_set(bit, 0);  /* revert before next probe */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* All three together (maybe DE+RE are split across two bits) */
    for (size_t i = 0; i < probe_n; i++) bsp_io_expander_set(probe_bits[i], 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    {
        uint16_t m = 0;
        esp_err_t e = modbus_client_read_coils(1, 0, 1, &m);
        ESP_LOGI(TAG, "probe EXIO0+6+7=1 slave=1 -> %s", esp_err_to_name(e));
        if (e == ESP_OK) { found_bit = 99; goto probe_done; }
    }
    /* No combination worked: leave all unused bits LOW to avoid surprises */
    for (size_t i = 0; i < probe_n; i++) bsp_io_expander_set(probe_bits[i], 0);

probe_done:
    if (found_bit == -2) {
        ESP_LOGI(TAG, "**** RS-485 works with default shadow; DE not on CH422G ****");
    } else if (found_bit == 99) {
        ESP_LOGI(TAG, "**** RS-485 works with EXIO0+6+7 all HIGH (combined) ****");
    } else if (found_bit >= 0) {
        ESP_LOGI(TAG, "**** RS-485 DE FOUND on CH422G EXIO%d ****", found_bit);
    } else {
        ESP_LOGW(TAG, "**** RS-485 DE probe FAILED on all CH422G bits ****");
        ESP_LOGW(TAG, "     DE is likely a direct ESP32 GPIO we haven't identified,");
        ESP_LOGW(TAG, "     or the transceiver needs UART_MODE_RS485_HALF_DUPLEX.");
    }
    ESP_LOGI(TAG, "---- DE probe done ----");

    /* Follow-up: full slave scan 1..8 with whatever DE state we settled on */
    ESP_LOGI(TAG, "---- slave scan 1..8 @ 9600 8N1 ----");
    for (uint8_t a = 1; a <= 8; a++) {
        uint16_t m = 0;
        esp_err_t e = modbus_client_read_coils(a, 0, 2, &m);
        ESP_LOGI(TAG, "scan slave=%u -> %s (mask=0x%04x)",
                 a, esp_err_to_name(e), m);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "---- scan done ----");

    TickType_t next_poll      = xTaskGetTickCount();
    TickType_t next_sec_poll  = xTaskGetTickCount();
    TickType_t next_mirror    = xTaskGetTickCount() + pdMS_TO_TICKS(50);
    for (;;) {
        /* Pause gate: if the UI (or anyone else) has asked us to stand
         * down, acknowledge and spin without touching the bus until
         * released. The caller then owns the UART exclusively. */
        if (s_pause_req) {
            s_paused_ack = true;
            while (s_pause_req) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            s_paused_ack = false;
            /* Reset pacing so we don't burst-catch-up after release. */
            next_poll     = xTaskGetTickCount();
            next_sec_poll = xTaskGetTickCount();
            next_mirror   = xTaskGetTickCount() + pdMS_TO_TICKS(50);
            continue;
        }

        mb_req_t req;
        TickType_t now      = xTaskGetTickCount();
        TickType_t wait_tk  = (int32_t)(next_poll - now) > 0 ? (next_poll - now) : 0;

        /* Clamp the wait so the colour wheel's latency stays under ~120 ms
         * regardless of what MB_POLL_INTERVAL_MS is set to. The wait is
         * only effective if neither the coil queue nor any RGB slot has
         * work -- any UI tap returns us early via xQueueReceive anyway. */
        const TickType_t max_wait = pdMS_TO_TICKS(120);
        if (wait_tk > max_wait) wait_tk = max_wait;

        if (xQueueReceive(s_q, &req, wait_tk) == pdTRUE) {
            handle_write(&req);
            /* Drain any additional queued writes so bursts (the user
             * mashing toggles) go out back-to-back. */
            while (xQueueReceive(s_q, &req, 0) == pdTRUE) {
                handle_write(&req);
            }
            drain_rgb_slots();    /* also push any fresh colours */
            continue;
        }

        /* Wait expired with no coil request. If any RGB slot is dirty
         * flush it first; only poll when nothing is pending. */
        drain_rgb_slots();

        if ((int32_t)(xTaskGetTickCount() - next_poll) >= 0) {
            poll_state();
            next_poll = xTaskGetTickCount() + pdMS_TO_TICKS(MB_POLL_INTERVAL_MS);
        }

        /* Dual-HMI: probe secondary's intent ring often (user-taps latency)
         * and push the mirror less often (secondary just needs periodic
         * state sync; its UI redraw is driven by the mirror_seq counter).
         *
         * If the secondary is currently flagged offline (after
         * MB_SEC_OFFLINE_THR consecutive failures), throttle both
         * transactions to MB_SEC_BACKOFF_MS to keep the bus clear for the
         * relay. Each FC10 push is ~140 ms of bus time at 9600 baud and
         * was stomping on the relay's reply window. */
        bool sec_online = hmi_sync_secondary_online();
        uint32_t sec_poll_period   = sec_online ? MB_SEC_POLL_MS   : MB_SEC_BACKOFF_MS;
        uint32_t sec_mirror_period = sec_online ? MB_MIRROR_PUSH_MS : MB_SEC_BACKOFF_MS;
        if ((int32_t)(xTaskGetTickCount() - next_sec_poll) >= 0) {
            uint16_t prev_seq = hmi_sync_primary_last_seq();
            poll_secondary_intents();
            next_sec_poll = xTaskGetTickCount() + pdMS_TO_TICKS(sec_poll_period);
            /* If new intents were applied (last_seq advanced), force the
             * next mirror push to fire on the very next loop iteration
             * so the secondary's UI confirms within ~100 ms instead of
             * waiting up to MB_MIRROR_PUSH_MS for the next scheduled
             * push. */
            if (hmi_sync_primary_last_seq() != prev_seq) {
                next_mirror = xTaskGetTickCount();
            }
        }
        if ((int32_t)(xTaskGetTickCount() - next_mirror) >= 0) {
            push_mirror_to_secondary();
            next_mirror = xTaskGetTickCount() + pdMS_TO_TICKS(sec_mirror_period);
        }
    }
}

esp_err_t modbus_task_start(void)
{
    if (s_task_handle) return ESP_OK;
    s_q = xQueueCreate(MB_QUEUE_LEN, sizeof(mb_req_t));
    if (!s_q) return ESP_ERR_NO_MEM;

    /* Pin to core 1 so the RGB LCD + LVGL on core 0 are never blocked
     * by UART waits. Priority 5 is below LVGL's typical priority. */
    BaseType_t ok = xTaskCreatePinnedToCore(modbus_worker, "modbus",
                                            4096, NULL, 5, &s_task_handle, 1);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "worker started on core 1, poll=%d ms", MB_POLL_INTERVAL_MS);
    return ESP_OK;
}
