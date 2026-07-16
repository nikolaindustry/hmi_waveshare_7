#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Wireless dual-HMI link (ESP-NOW transport).
 *
 * Carries the exact same two hmi_sync blocks the RS-485 path moves:
 *
 *   INTENT  secondary -> primary   one ring slot per frame (10 B)
 *   MIRROR  primary  -> secondary  full 64-reg state block (~136 B)
 *
 * The sync layer (hmi_sync.c) is untouched: on the wireless
 * secondary a drain task reads the same intent ring the wired
 * modbus_server exposes, and received mirrors go through
 * hmi_sync_write_mirror() exactly like an FC 0x10 push. The UI's
 * link pill keeps working unchanged because it is driven by
 * hmi_sync_mirror_age_ms(), not by the transport.
 *
 * Pairing: user opens a 60 s window on the PRIMARY (Maintenance ->
 * HMI Role -> PAIR). An unpaired wireless secondary channel-hops,
 * broadcasting PAIR_REQ; the primary answers with the peer key; both
 * persist the binding in NVS. All post-pairing traffic is encrypted
 * (ESP-NOW CCMP with a per-pair random LMK).
 * ------------------------------------------------------------------ */

/* PRIMARY side. Call after hyperwisor_start(); waits in a background
 * task for WiFi to come up (or brings up a minimal STA if the cloud
 * stack never does), then serves paired wireless secondaries. */
esp_err_t hmi_link_primary_start(void);

/* WIRELESS SECONDARY side. Owns the WiFi bring-up (STA, never
 * connects to an AP). Auto-pairs if unpaired, then syncs. */
esp_err_t hmi_link_secondary_start(void);

/* Open the pairing window (PRIMARY) for `seconds`. While open, the
 * first PAIR_REQ heard is accepted and replaces any stored peer. */
void hmi_link_pair_begin(uint32_t seconds);

/* Forget the stored peer binding. On the secondary this drops the
 * link and re-enters the pairing scan automatically. */
void hmi_link_forget(void);

/* Status for the Maintenance UI. */
bool hmi_link_paired(void);
bool hmi_link_pairing_active(void);
/* Writes "aa:bb:cc:dd:ee:ff" (or "--") into buf. */
void hmi_link_peer_str(char *buf, size_t len);

/* Signal strength of the peer's most recent frame, in dBm (typically
 * -30 .. -90; lower/more-negative = weaker). Returns false if nothing
 * has been heard from the paired peer in the last 5 s (link is down
 * or was never up), in which case *out_dbm is left unset. */
bool hmi_link_peer_rssi(int8_t *out_dbm);

#ifdef __cplusplus
}
#endif
