/* =====================================================================
 *  hmi_link.c — ESP-NOW transport for the dual-HMI intent/mirror sync.
 *
 *  See hmi_link.h for the architecture. Design rules:
 *    - hmi_sync.c is the single source of truth for block layout;
 *      this file only moves bytes.
 *    - The RX callback runs in the WiFi task: it validates and queues,
 *      nothing else. All state changes happen in the link task.
 *    - Loss tolerance comes from the sync model (repeated mirrors,
 *      re-tappable intents); the link adds cheap 2x intent sends and
 *      dedup, not a reliability protocol.
 * ===================================================================== */

#include "hmi_link.h"
#include "hmi_sync.h"
#include "hmi_role.h"
#include "modbus_task.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "hmi_link";

/* ---- wire format ---------------------------------------------------- */

#define LINK_MAGIC        0x48574C31u   /* "HWL1" */
#define LINK_T_INTENT     1
#define LINK_T_MIRROR     2
#define LINK_T_PAIR_REQ   3
#define LINK_T_PAIR_ACK   4

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  rsvd;
    uint16_t seq;
} link_hdr_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint16_t   cmd, a0, a1, a2;
} link_intent_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint16_t   regs[HMI_MIRROR_REGS];
} link_mirror_t;

typedef struct __attribute__((packed)) {
    link_hdr_t hdr;
    uint8_t    target[6];               /* which secondary this ACK is for */
    uint8_t    lmk[16];                 /* per-pair encryption key         */
    uint8_t    channel;                 /* primary's current WiFi channel  */
} link_pair_ack_t;

/* Encryption: global PMK (compile-time) + per-pair random LMK exchanged
 * once during the explicit pairing window. Pairing frames go in clear on
 * broadcast; everything after is CCMP-encrypted unicast. */
static const uint8_t s_pmk[16] = {
    0x6e, 0x69, 0x6b, 0x6f, 0x6c, 0x61, 0x2d, 0x68,
    0x6d, 0x69, 0x2d, 0x70, 0x6d, 0x6b, 0x30, 0x31,
};

/* ---- state ----------------------------------------------------------- */

#define LINK_NVS_NS       "hmilink"
#define LINK_TICK_MS      50
#define MIRROR_PERIOD_MS  400          /* heartbeat when state is idle    */
#define PAIR_HOP_MS       300          /* dwell per channel while scanning */
#define LINK_STALE_MS     5000         /* mirror silence -> rescan channel */

typedef struct {
    uint8_t  mac[6];
    uint8_t  type;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t  payload[sizeof(link_mirror_t)];  /* largest frame */
} rx_msg_t;

static QueueHandle_t   s_rxq;
static bool            s_paired;
static uint8_t         s_peer[6];
static uint8_t         s_lmk[16];
static uint8_t         s_channel = 1;
static volatile int8_t s_peer_rssi     = 0;   /* dBm from the peer's last frame  */
static volatile int64_t s_peer_rssi_us = 0;   /* esp_timer_get_time() of that RX */
static volatile int64_t s_pair_until_us;      /* pairing window (primary)  */
static uint16_t        s_tx_seq;              /* link-level frame counter  */
static uint16_t        s_last_rx_intent_seq;  /* dedup (primary)           */
static bool            s_rx_intent_seen;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- NVS binding ------------------------------------------------------ */

static void binding_load(void)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t ml = sizeof(s_peer), kl = sizeof(s_lmk);
    if (nvs_get_blob(h, "peer", s_peer, &ml) == ESP_OK && ml == 6 &&
        nvs_get_blob(h, "lmk",  s_lmk,  &kl) == ESP_OK && kl == 16) {
        s_paired = true;
        nvs_get_u8(h, "chan", &s_channel);
        ESP_LOGI(TAG, "binding loaded: peer=%02x:%02x:%02x:%02x:%02x:%02x chan=%u",
                 s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5],
                 s_channel);
    }
    nvs_close(h);
}

static void binding_save(void)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "peer", s_peer, 6);
    nvs_set_blob(h, "lmk",  s_lmk, 16);
    nvs_set_u8  (h, "chan", s_channel);
    nvs_commit(h);
    nvs_close(h);
}

void hmi_link_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(LINK_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_paired) esp_now_del_peer(s_peer);
    s_paired = false;
    ESP_LOGW(TAG, "peer binding forgotten");
}

bool hmi_link_paired(void)         { return s_paired; }
bool hmi_link_pairing_active(void) { return esp_timer_get_time() < s_pair_until_us; }

void hmi_link_pair_begin(uint32_t seconds)
{
    s_pair_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGW(TAG, "pairing window open for %lus", (unsigned long)seconds);
}

void hmi_link_peer_str(char *buf, size_t len)
{
    if (!s_paired) { snprintf(buf, len, "--"); return; }
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
}

bool hmi_link_peer_rssi(int8_t *out_dbm)
{
    if (!s_paired || s_peer_rssi_us == 0) return false;
    if (esp_timer_get_time() - s_peer_rssi_us > 5 * 1000000) return false;
    if (out_dbm) *out_dbm = s_peer_rssi;
    return true;
}

/* ---- ESP-NOW plumbing -------------------------------------------------- */

/* ESP-NOW peers are bound to one WiFi interface (STA or AP); a send to
 * a peer registered on the wrong one fails with ESP_ERR_ESPNOW_IF even
 * though the radio is otherwise fine. A brand-new, unprovisioned unit
 * has no cloud credentials yet, so hyperwisor's WiFi stack sits in
 * SoftAP (its onboarding hotspot) rather than STA -- local wireless
 * pairing must keep working in that state, not just after the van's
 * owner has set up the cloud connection. Always ask the radio which
 * interface is actually live rather than assuming STA. */
static wifi_interface_t current_ifidx(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    return (mode == WIFI_MODE_AP) ? WIFI_IF_AP : WIFI_IF_STA;
}

static wifi_interface_t s_peer_ifidx = WIFI_IF_STA;

static void add_peer(const uint8_t mac[6], const uint8_t *lmk)
{
    s_peer_ifidx = current_ifidx();

    esp_now_peer_info_t p = { 0 };
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;                       /* follow current radio channel */
    p.ifidx   = s_peer_ifidx;
    if (lmk) {
        p.encrypt = true;
        memcpy(p.lmk, lmk, 16);
    }
    esp_now_del_peer(mac);               /* replace if it already exists */
    esp_err_t err = esp_now_add_peer(&p);
    if (err != ESP_OK) ESP_LOGE(TAG, "add_peer: %s", esp_err_to_name(err));
}

/* Call once per loop tick on both roles. If the radio's interface
 * changed since peers were last registered (e.g. SoftAP provisioning
 * finished and the stack switched to STA), every previously-added peer
 * is now on the wrong interface and silently fails -- re-add them. */
static void resync_peer_ifidx(void)
{
    if (current_ifidx() == s_peer_ifidx) return;
    ESP_LOGI(TAG, "WiFi interface changed (AP<->STA) -- re-registering ESP-NOW peers");
    add_peer(BCAST, NULL);
    if (s_paired) add_peer(s_peer, s_lmk);
}

static void rx_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(link_hdr_t)) return;
    const link_hdr_t *h = (const link_hdr_t *)data;
    if (h->magic != LINK_MAGIC) return;

    if (s_paired && memcmp(info->src_addr, s_peer, 6) == 0 && info->rx_ctrl) {
        s_peer_rssi    = info->rx_ctrl->rssi;
        s_peer_rssi_us = esp_timer_get_time();
    }

    rx_msg_t m;
    memcpy(m.mac, info->src_addr, 6);
    m.type = h->type;
    m.seq  = h->seq;
    m.payload_len = (uint16_t)len;
    if (len > (int)sizeof(m.payload)) return;
    memcpy(m.payload, data, len);
    /* Queue full = drop; the next mirror/heartbeat repairs any loss. */
    if (s_rxq) xQueueSend(s_rxq, &m, 0);
}

static void send_frame(const uint8_t *mac, const void *frame, size_t len)
{
    esp_err_t err = esp_now_send(mac, (const uint8_t *)frame, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
    }
}

/* esp_now_send() returning ESP_OK only means the radio accepted the
 * frame for transmission, not that the peer heard it -- ESP-NOW unicast
 * is 802.11-ACKed, and this callback reports the real outcome. A string
 * of FAILED here (not a one-off) means the two radios are not actually
 * within range on the same channel, even though pairing succeeded. */
static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        const uint8_t *d = info->des_addr;
        ESP_LOGW(TAG, "esp_now send FAILED (no ACK) to %02x:%02x:%02x:%02x:%02x:%02x",
                 d[0], d[1], d[2], d[3], d[4], d[5]);
    }
}

static esp_err_t espnow_up(void)
{
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err)); return err; }
    esp_now_set_pmk(s_pmk);
    esp_now_register_send_cb(send_cb);
    esp_now_register_recv_cb(rx_cb);
    add_peer(BCAST, NULL);
    /* Modem power save silently drops ESP-NOW RX between DTIM beacons.
     * Both units are dock/harness powered in practice, so trade the
     * ~100 mA for a link that actually hears its peer. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

/* Bring up a minimal STA (never connects) — used by the wireless
 * secondary, and by the primary only if the cloud stack never started
 * WiFi. Idempotent-ish: tolerates an already-created loop/netif. */
static esp_err_t wifi_min_up(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    return esp_wifi_start();
}

static bool wifi_is_started(void)
{
    uint8_t primary;
    wifi_second_chan_t second;
    return esp_wifi_get_channel(&primary, &second) == ESP_OK;
}

/* ---- PRIMARY ----------------------------------------------------------- */

static void primary_handle_rx(const rx_msg_t *m)
{
    if (m->type == LINK_T_INTENT && m->payload_len >= sizeof(link_intent_t)) {
        if (!s_paired || memcmp(m->mac, s_peer, 6) != 0) return;
        /* Dedup: intents are sent 2x for loss resilience. */
        if (s_rx_intent_seen && (int16_t)(m->seq - s_last_rx_intent_seq) <= 0) return;
        s_last_rx_intent_seq = m->seq;
        s_rx_intent_seen     = true;

        const link_intent_t *it = (const link_intent_t *)m->payload;
        ESP_LOGI(TAG, "intent rx: cmd=%u a0=%u a1=%u a2=%u",
                 it->cmd, it->a0, it->a1, it->a2);
        hmi_sync_apply_intent((hmi_cmd_t)it->cmd, it->a0, it->a1, it->a2);
        return;
    }

    if (m->type == LINK_T_PAIR_REQ) {
        if (!hmi_link_pairing_active()) return;
        s_pair_until_us = 0;                       /* single-shot window */

        memcpy(s_peer, m->mac, 6);
        esp_fill_random(s_lmk, sizeof(s_lmk));

        uint8_t ch; wifi_second_chan_t sc;
        esp_wifi_get_channel(&ch, &sc);
        s_channel = ch;
        binding_save();

        /* ACK rides broadcast (never encrypted) so the not-yet-keyed
         * secondary can read it; it carries the target MAC + LMK. */
        link_pair_ack_t ack = {
            .hdr = { LINK_MAGIC, LINK_T_PAIR_ACK, 0, ++s_tx_seq },
            .channel = ch,
        };
        memcpy(ack.target, m->mac, 6);
        memcpy(ack.lmk, s_lmk, 16);
        send_frame(BCAST, &ack, sizeof(ack));

        add_peer(s_peer, s_lmk);
        s_paired = true;
        s_rx_intent_seen = false;
        ESP_LOGW(TAG, "paired with %02x:%02x:%02x:%02x:%02x:%02x",
                 s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5]);
    }
}

static void primary_task(void *arg)
{
    (void)arg;

    /* The cloud stack owns WiFi bring-up; wait for it. If it never
     * arrives (no hyperwisor / init failure) run our own bare STA. */
    for (int i = 0; i < 120 && !wifi_is_started(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!wifi_is_started() && wifi_min_up() != ESP_OK) {
        ESP_LOGE(TAG, "no WiFi available -- wireless link disabled");
        vTaskDelete(NULL);
        return;
    }
    if (espnow_up() != ESP_OK) { vTaskDelete(NULL); return; }
    if (s_paired) add_peer(s_peer, s_lmk);
    ESP_LOGI(TAG, "primary link up (paired=%d)", (int)s_paired);

    uint32_t   last_state_seq = 0;
    TickType_t next_mirror    = xTaskGetTickCount();

    for (;;) {
        resync_peer_ifidx();

        rx_msg_t m;
        while (xQueueReceive(s_rxq, &m, 0) == pdTRUE) primary_handle_rx(&m);

        if (s_paired) {
            uint32_t seq = modbus_task_state_seq();
            bool due = (int32_t)(xTaskGetTickCount() - next_mirror) >= 0;
            if (seq != last_state_seq || due) {
                last_state_seq = seq;
                next_mirror    = xTaskGetTickCount() + pdMS_TO_TICKS(MIRROR_PERIOD_MS);

                link_mirror_t mir = { .hdr = { LINK_MAGIC, LINK_T_MIRROR, 0, ++s_tx_seq } };
                hmi_sync_encode_mirror(mir.regs);
                send_frame(s_peer, &mir, sizeof(mir));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_TICK_MS));
    }
}

esp_err_t hmi_link_primary_start(void)
{
    if (hmi_role_get() != HMI_ROLE_PRIMARY) return ESP_ERR_INVALID_STATE;
    s_rxq = xQueueCreate(8, sizeof(rx_msg_t));
    if (!s_rxq) return ESP_ERR_NO_MEM;
    binding_load();
    return xTaskCreatePinnedToCore(primary_task, "hmi_link", 4096, NULL, 5, NULL, 1)
           == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ---- WIRELESS SECONDARY ------------------------------------------------- */

static void secondary_handle_rx(const rx_msg_t *m, bool *mirror_rx)
{
    if (m->type == LINK_T_MIRROR && m->payload_len >= sizeof(link_mirror_t)) {
        if (!s_paired || memcmp(m->mac, s_peer, 6) != 0) return;
        const link_mirror_t *mir = (const link_mirror_t *)m->payload;
        hmi_sync_write_mirror(mir->regs);
        *mirror_rx = true;
        /* Throttled: heartbeat is ~2.5 Hz, log every 20th (~8 s) so the
         * link stays visible in the log without flooding it. */
        static uint32_t s_mirror_rx_count;
        if ((++s_mirror_rx_count % 20) == 1) {
            ESP_LOGI(TAG, "mirror rx seq=%u (count=%lu)", m->seq,
                     (unsigned long)s_mirror_rx_count);
        }
        return;
    }

    if (m->type == LINK_T_PAIR_ACK && m->payload_len >= sizeof(link_pair_ack_t)) {
        uint8_t my_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, my_mac);
        const link_pair_ack_t *ack = (const link_pair_ack_t *)m->payload;
        if (memcmp(ack->target, my_mac, 6) != 0) return;   /* not for us */

        memcpy(s_peer, m->mac, 6);
        memcpy(s_lmk, ack->lmk, 16);
        s_channel = ack->channel;
        binding_save();
        add_peer(s_peer, s_lmk);
        s_paired = true;
        ESP_LOGW(TAG, "paired to primary %02x:%02x:%02x:%02x:%02x:%02x chan=%u",
                 s_peer[0], s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5],
                 s_channel);
    }
}

/* Drain new intent-ring slots exactly the way the wired primary's
 * FC 0x03 poll consumes them, but locally: diff head_seq against what
 * we've already transmitted and forward only the fresh slots. */
static void secondary_drain_intents(uint16_t *last_sent_seq, bool *seq_valid)
{
    uint16_t blk[HMI_INTENT_RING_REGS];
    hmi_sync_read_intent_block(blk);
    uint16_t head = blk[0];

    if (!*seq_valid) { *last_sent_seq = head; *seq_valid = true; return; }
    if (head == *last_sent_seq) return;

    uint16_t delta = (uint16_t)(head - *last_sent_seq);
    if (delta > HMI_INTENT_SLOT_COUNT) delta = HMI_INTENT_SLOT_COUNT;

    for (uint16_t i = HMI_INTENT_SLOT_COUNT - delta; i < HMI_INTENT_SLOT_COUNT; i++) {
        const uint16_t *slot = &blk[2 + i * HMI_INTENT_REGS_PER_SLOT];
        link_intent_t f = {
            .hdr = { LINK_MAGIC, LINK_T_INTENT, 0, ++s_tx_seq },
            .cmd = slot[0], .a0 = slot[1], .a1 = slot[2], .a2 = slot[3],
        };
        /* 2x send: cheap insurance against a lost frame; the primary
         * dedups on hdr.seq. */
        send_frame(s_peer, &f, sizeof(f));
        send_frame(s_peer, &f, sizeof(f));
    }
    *last_sent_seq = head;
}

static void secondary_task(void *arg)
{
    (void)arg;

    if (wifi_min_up() != ESP_OK || espnow_up() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi/ESP-NOW bring-up failed -- link disabled");
        vTaskDelete(NULL);
        return;
    }
    if (s_paired) {
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        add_peer(s_peer, s_lmk);
    }
    ESP_LOGI(TAG, "secondary link up (paired=%d chan=%u)", (int)s_paired, s_channel);

    uint16_t   last_sent_seq = 0;
    bool       seq_valid     = false;
    uint8_t    hop           = s_channel;
    TickType_t next_hop      = xTaskGetTickCount();

    for (;;) {
        resync_peer_ifidx();

        bool mirror_rx = false;
        rx_msg_t m;
        while (xQueueReceive(s_rxq, &m, 0) == pdTRUE) {
            secondary_handle_rx(&m, &mirror_rx);
        }

        if (mirror_rx) {
            /* Locked on: remember the working channel if it drifted. */
            uint8_t ch; wifi_second_chan_t sc;
            if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && ch != s_channel) {
                s_channel = ch;
                binding_save();
            }
        } else {
            /* Hunt mode: unpaired (broadcast PAIR_REQ per hop) or the
             * paired link has gone quiet (primary rebooted / router
             * moved channels) -> sweep 1..13 until mirrors reappear. */
            bool stale = hmi_sync_mirror_age_ms() > LINK_STALE_MS;
            if ((!s_paired || stale) &&
                (int32_t)(xTaskGetTickCount() - next_hop) >= 0) {
                next_hop = xTaskGetTickCount() + pdMS_TO_TICKS(PAIR_HOP_MS);
                hop = (hop % 13) + 1;
                esp_wifi_set_channel(hop, WIFI_SECOND_CHAN_NONE);
                if (!s_paired) {
                    link_hdr_t req = { LINK_MAGIC, LINK_T_PAIR_REQ, 0, ++s_tx_seq };
                    send_frame(BCAST, &req, sizeof(req));
                }
            }
        }

        if (s_paired) secondary_drain_intents(&last_sent_seq, &seq_valid);

        vTaskDelay(pdMS_TO_TICKS(LINK_TICK_MS));
    }
}

esp_err_t hmi_link_secondary_start(void)
{
    if (hmi_role_get() != HMI_ROLE_SECONDARY_WIRELESS) return ESP_ERR_INVALID_STATE;
    s_rxq = xQueueCreate(8, sizeof(rx_msg_t));
    if (!s_rxq) return ESP_ERR_NO_MEM;
    binding_load();
    return xTaskCreatePinnedToCore(secondary_task, "hmi_link", 4096, NULL, 5, NULL, 1)
           == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
