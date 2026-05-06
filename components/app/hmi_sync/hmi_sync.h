#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Dual-HMI sync contract.
 *
 * The secondary HMI acts as a Modbus slave at HMI_SECONDARY_SLAVE_ID
 * (0x10). It exposes two holding-register blocks:
 *
 *   INTENT RING (base 0x0000, 34 regs) - secondary -> primary
 *     0x0000  head_seq (u16, wraps)
 *     0x0001  ring_depth (= 8, constant)
 *     0x0002..0x0021  8 slots * 4 regs each: cmd, a0, a1, a2
 *
 *   STATE MIRROR (base 0x0100, 64 regs) - primary -> secondary
 *     See mirror_offsets in hmi_sync.c for exact layout.
 *
 * The primary polls head_seq every 100 ms (FC 0x03) and pushes a
 * fresh mirror every 500 ms (FC 0x10). Both sides agree on this
 * header.
 * ------------------------------------------------------------------ */

#define HMI_SECONDARY_SLAVE_ID      0x10

#define HMI_INTENT_BASE             0x0000u
#define HMI_INTENT_HEAD_ADDR        0x0000u
#define HMI_INTENT_DEPTH_ADDR       0x0001u
#define HMI_INTENT_SLOTS_BASE       0x0002u
#define HMI_INTENT_SLOT_COUNT       8
#define HMI_INTENT_REGS_PER_SLOT    4
#define HMI_INTENT_RING_REGS        (2 + HMI_INTENT_SLOT_COUNT * HMI_INTENT_REGS_PER_SLOT) /* 34 */

#define HMI_MIRROR_BASE             0x0100u
#define HMI_MIRROR_REGS             64

typedef enum {
    HMI_CMD_NONE     = 0,
    HMI_CMD_COIL_SET = 1,  /* a0 = coil index (0..15), a1 = on (0/1)             */
    HMI_CMD_RGB_SET  = 2,  /* a0 = zone (0=roof,1=floor),
                              a1 = hue (0..359),
                              a2 = (on<<15) | (sat<<8) | brightness              */
    HMI_CMD_AC_SET   = 3,  /* a0 = ac field id,  a1 = value                      */
    HMI_CMD_STAR_SET = 4,  /* a0 = on (0/1),     a1 = intensity (0..100)         */
} hmi_cmd_t;

typedef enum {
    HMI_AC_FIELD_POWER     = 0,
    HMI_AC_FIELD_AUTO      = 1,
    HMI_AC_FIELD_FAN_LEVEL = 2,
    HMI_AC_FIELD_TEMP      = 3,
    HMI_AC_FIELD_AIRFLOW   = 4,   /* value = ac_airflow_bit_t mask */
    HMI_AC_FIELD_FLAGS     = 5,   /* bit0=front_def, bit1=rear_def, bit2=fresh   */
} hmi_ac_field_t;

/* ---- SECONDARY SIDE -------------------------------------------------- */

/* Append an intent to the ring (called from UI handlers on secondary).
 * Wraps silently if the ring is full; the head_seq bump still lets the
 * primary know something was dropped. */
void hmi_sync_push_intent(hmi_cmd_t cmd, uint16_t a0, uint16_t a1, uint16_t a2);

/* Snapshot the entire intent ring block (for the slave server's FC 0x03). */
void hmi_sync_read_intent_block(uint16_t out[HMI_INTENT_RING_REGS]);

/* Apply a mirror block received via FC 0x10 into ac_state/ctrl_state. */
void hmi_sync_write_mirror(const uint16_t in[HMI_MIRROR_REGS]);

/* True iff the secondary has received at least one mirror push. */
bool hmi_sync_mirror_seen(void);

/* Monotonic counter bumped on each mirror receipt -- UI polls this. */
uint32_t hmi_sync_mirror_seq(void);

/* Milliseconds since the last mirror push was received (UINT32_MAX if
 * never). Used to drive the link-online indicator on the secondary. */
uint32_t hmi_sync_mirror_age_ms(void);

/* ---- PRIMARY SIDE ---------------------------------------------------- */

/* Encode current local state into a mirror block. */
void hmi_sync_encode_mirror(uint16_t out[HMI_MIRROR_REGS]);

/* Dispatch a received intent into local state + transports. */
void hmi_sync_apply_intent(hmi_cmd_t cmd, uint16_t a0, uint16_t a1, uint16_t a2);

/* Primary's last-seen head_seq tracker (drives the ring poll loop). */
uint16_t hmi_sync_primary_last_seq(void);
void     hmi_sync_primary_set_last_seq(uint16_t seq);

/* Called by the primary's modbus_task when poll of secondary succeeds /
 * fails, so the secondary-online pill on the primary's UI has a signal
 * independent of the relay-board link. */
void hmi_sync_primary_note_secondary(bool online);
bool hmi_sync_secondary_online(void);

#ifdef __cplusplus
}
#endif
