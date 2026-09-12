#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------
 * Control-tab state model.
 *
 * Everything is kept behind accessor functions + exposed arrays so
 * the UI can walk a list of "items" without caring how many there
 * are or what they are called.  Adding a new reading light or switch
 * at a later date means editing the arrays in ctrl_state.c -- NO UI
 * change is required.
 *
 * `name` is a fixed-size char buffer (not a pointer to literal) so
 * the label can be mutated at runtime later, for example loaded from
 * NVS or renamed via a settings screen.
 * ------------------------------------------------------------------ */

#define CTRL_NAME_MAX 24

/* 0xFFFF sentinel = "not mapped to any Modbus coil" */
#define CTRL_COIL_NONE 0xFFFFu

typedef struct {
    char     name[CTRL_NAME_MAX];
    bool     on;        /* desired/displayed state */
    bool     pending;   /* true while a Modbus write is in flight */
    uint16_t coil;      /* Modbus coil address, or CTRL_COIL_NONE */

    /* ---- per-tile behaviour, set from the Maintenance tab ----
     *
     * momentary: push-to-on, release-to-off. The coil is energised
     *   only while a finger is on the tile. For horn, washer pump,
     *   step motors, window motors -- anything where "latched ON"
     *   is the wrong default and possibly a hazard.
     *   Default false = the normal latching toggle.
     *
     * home: also show this tile as a quick button on the Overview
     *   (home) screen, so the driver does not have to go into the
     *   Control tab for the things they touch every trip.
     *   At most CTRL_HOME_MAX tiles across BOTH groups. */
    bool     momentary;
    bool     home;

    /* True between press and release of a momentary tile. Only these
     * are released by ctrl_momentary_release_all(), so leaving a tab
     * does not spam the bus with writes for tiles nobody touched. */
    bool     held;
} ctrl_toggle_t;

/* How many quick buttons fit on the Overview screen. The greeting
 * column is 340 px wide and the tiles are 78 px square with 9 px
 * gaps: 4*78 + 3*9 = 339. One row is all the vertical room there is
 * under the owner name, so this is a hard geometric limit, not a
 * policy choice. */
#define CTRL_HOME_MAX 4

/* RGB zone: hue/sat come from the colorwheel, brightness is a
 * separate slider, master on/off is a pill switch. */
typedef struct {
    uint16_t hue;        /* 0..359 */
    uint8_t  sat;        /* 0..100 */
    uint8_t  brightness; /* 0..100 */
    bool     on;
} ctrl_rgb_t;

/* Ambient fibre-optic star roof. Driven by a plain on/off relay coil,
 * so `intensity` is currently cosmetic -- a relay cannot dim. Kept in
 * the struct (and in the HMI-to-HMI mirror) so a dimmable illuminator
 * can be supported later without a protocol change. */
typedef struct {
    bool    on;
    uint8_t intensity; /* 0..100 -- not wired to hardware yet */
} ctrl_star_t;

/* Relay coil for the star roof on the Waveshare 16CH board (slave 0x01).
 *
 * Coil budget on that board: 0..7 reading lights, 8/9/10 HVAC fan taps,
 * 12 HVAC compressor clutch. 15 was nominally "Switch 8" but is not
 * driven by HVAC, so it is the safest free channel; Switch 8 has been
 * unmapped (CTRL_COIL_NONE) to keep exactly one owner per coil. */
#define CTRL_STAR_COIL  15u

/* Load the persisted RGB / star-roof settings from NVS. Call at boot
 * BEFORE the display comes up, alongside the other NVS loads, so the
 * first paint already shows the saved colours. Relay coil states are
 * NOT persisted -- those are read back from the relay board itself. */
void ctrl_state_init(void);

/* Start the background writer that re-saves those settings whenever
 * they settle on a new value. Creates an LVGL timer, so this must run
 * AFTER LVGL is initialised (i.e. after bsp_init()). */
void ctrl_state_start_autosave(void);

/* Singletons ------------------------------------------------------- */
ctrl_rgb_t    *ctrl_rgb_roof(void);
ctrl_rgb_t    *ctrl_rgb_floor(void);
ctrl_star_t   *ctrl_star_roof(void);

/* Driving tiles ---------------------------------------------------- */

/* Write one tile to `want`: enqueues a Modbus coil write and marks the
 * tile pending, or just flips `on` for an unmapped tile. Shared by the
 * Control tab and the Overview quick buttons so both behave identically.
 * Safe to call with item == NULL. */
void ctrl_toggle_drive(ctrl_toggle_t *item, bool want);

/* Press and release a momentary tile. Always use these rather than
 * ctrl_toggle_drive() directly, because holding a coil down safely
 * needs more than two writes once the HMI is wireless:
 *
 *  - On a SECONDARY the press is an ESP-NOW intent, and the matching
 *    release is another one. If that release packet is lost the coil
 *    stays energised with nobody's finger on it. So the secondary
 *    re-asserts the hold every CTRL_HOLD_REFRESH_MS while pressed, and
 *    the primary drops any remotely-held coil that has not been
 *    re-asserted within CTRL_HOLD_DEADMAN_MS. Losing the link now
 *    fails safe -- the coil falls out within about a second -- instead
 *    of failing on.
 *  - On a PRIMARY the LVGL press/release events are reliable, so the
 *    repeat is skipped and these are a plain pair of writes.
 *
 * Only one tile can be held at a time; a second press releases the
 * first, matching what a single finger can physically do. */
void ctrl_momentary_hold_begin(ctrl_toggle_t *item);
void ctrl_momentary_hold_end(void);

/* Apply a coil intent received from a secondary. `on` true both starts
 * and refreshes the deadman; false releases immediately. Returns true
 * if the coil belonged to a momentary tile and was handled here. */
bool ctrl_momentary_remote(uint16_t coil, bool on);

/* How often a held secondary re-asserts, and how long the primary
 * waits before assuming the finger (or the link) is gone. The gap
 * between them absorbs a few dropped packets without chattering. */
#define CTRL_HOLD_REFRESH_MS  250
#define CTRL_HOLD_DEADMAN_MS  900

/* De-energise every momentary tile currently held down.
 *
 * A momentary coil must never outlive the finger holding it. LVGL will
 * not deliver a RELEASED event if the tile is destroyed mid-press (tab
 * switch, screen rebuild), so every screen that drives momentary tiles
 * calls this from its delete handler. Idempotent. */
void ctrl_momentary_release_all(void);

/* Arrays ----------------------------------------------------------- */
/* Both return a pointer to the first element; count is written
 * through out_count (may be NULL). */
ctrl_toggle_t *ctrl_reading_lights(int *out_count);
ctrl_toggle_t *ctrl_switches(int *out_count);
