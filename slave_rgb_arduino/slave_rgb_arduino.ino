/* =======================================================================
 *  Hyperwisor RGB Slave — ESP32 + MAX485 (HW-097)
 *  Addressable-LED edition (WS2812B / SK6812)
 *
 *  Role on the bus : Modbus RTU SLAVE
 *  Slave address   : 0x20 (32 decimal)
 *  Bus             : RS-485, 115200 8N1, same wires as the Waveshare
 *                    relay board (0x01).
 *  Master          : ESP32-S3 HMI (hyperwisor_s3).
 *
 *  This single slave drives TWO independent addressable LED zones:
 *    - ROOF  : LED strip behind the head-liner
 *    - FLOOR : LED strip in the floor / footwell channel
 *
 *  The Modbus register map is UNCHANGED from the old 3-wire PWM
 *  version, so the HMI master needs no firmware update at all — it
 *  still just writes one R/G/B/MODE per zone. What changed is what
 *  this slave DOES with that: instead of driving a single-color
 *  strip via 3 PWM channels, it now paints every individually
 *  addressable pixel (solid color for STATIC, a genuine moving
 *  rainbow gradient for RAINBOW — impossible on the old hardware).
 *
 *  Holding-register map (identical to the PWM version)
 *  -----------------------------------------------------
 *   ROOF ZONE   regs 0..5
 *     Reg 0  R          0..255
 *     Reg 1  G          0..255
 *     Reg 2  B          0..255
 *     Reg 3  MODE       0 STATIC, 1 RAINBOW, 2 OFF
 *     Reg 4  SPEED      1..255  ms between rainbow steps (lower=faster)
 *     Reg 5  BRIGHTNESS 0..255  global multiplier for this zone
 *
 *   FLOOR ZONE  regs 6..11  (same layout as above)
 *
 *   STRIP LENGTHS (new — addressable-LED only)
 *     Reg 12 ROOF  LED count  0..MAX_LEDS_PER_ZONE
 *     Reg 13 FLOOR LED count  0..MAX_LEDS_PER_ZONE
 *
 *  LED COUNT — owned by THIS board
 *  -----------------------------------------------
 *  How many LEDs are wired isn't known until the strip is cut to fit
 *  the vehicle, so it is not a compile-time constant. It is set here,
 *  over serial, and persists to flash (Preferences/NVS):
 *
 *      ROOF 1000    <Enter>   -- set roof chain to 1000 LEDs
 *      FLOOR 1000   <Enter>   -- set floor chain to 1000 LEDs
 *      COUNTS       <Enter>   -- print current counts
 *
 *  The SLAVE IS AUTHORITATIVE. Regs 12/13 report the current lengths
 *  so the HMI can display them, but a write from the bus is rejected
 *  and the real value is put straight back — see enforce_led_counts().
 *  This board is the one physically wired to the strip, so it is the
 *  only thing that can know the true length; letting a screen elsewhere
 *  overwrite it just risks truncating the strip to whatever limit that
 *  screen happens to have. It also means the length survives an HMI
 *  swap or the HMI being switched off entirely.
 *
 *  Hardware wiring
 *  ---------------
 *     ESP32           HW-097 (MAX485)
 *     GPIO 17  ──►    DI   (driver input, fed by Serial2 TX)
 *     GPIO 16  ◄──    RO   (receiver output, feeds Serial2 RX)
 *     GPIO 4   ──►    DE + RE tied together (direction control)
 *     3V3/5V   ──►    VCC
 *     GND      ──►    GND
 *     HW-097 A/B ──► bus A/B (same pair as relay board)
 *
 *     ESP32           LED strips -- one chain per zone
 *     GPIO 32  ──►    ROOF  strip DATA-IN
 *     GPIO 23  ──►    FLOOR strip DATA-IN
 *
 *     Up to 1000 individually addressable pixels per zone (2000 total).
 *
 *     5V/GND   ──►    strip power. Size the supply for the STRIP, not
 *                     the controller: ~60 mA per pixel at full white,
 *                     so 2000 pixels is ~100 A worst case. Real scenes
 *                     draw far less, but a 1000-pixel run MUST have
 *                     power injected every 1-2 m or the far end browns
 *                     out and drifts colour. Also: 300-500 ohm in
 *                     series with each DATA line, and a >=1000 uF cap
 *                     across 5V/GND at the strip's input end.
 *
 *  Libraries (Arduino Library Manager)
 *  ------------------------------------
 *   FastLED               (Daniel Garcia)
 *   Preferences            bundled with the ESP32 core
 *   emelianov / modbus-esp8266  (install as ZIP if Library Manager
 *   can't find it — works on ESP32 despite the name)
 * ======================================================================= */

#include <ModbusRTU.h>
#include <FastLED.h>
#include <Preferences.h>

/* ---- LED data pins: one output per zone ----
 *
 * One data pin per zone, each driving one continuous chain of up to
 * MAX_LEDS_PER_ZONE addressable pixels. (An earlier revision mirrored
 * each zone across three pins to multiply the LED count while keeping
 * chains short; that is no longer needed now the per-zone ceiling is
 * high enough to address every pixel individually.)
 *
 * NOT GPIO12: that is the MTDI strapping pin, sampled at reset to pick
 * the flash voltage. If anything on the strip side (a level shifter's
 * pull-up, a long floating lead) holds it high at power-on the module
 * won't boot. Also avoided: 6-11 (SPI flash), 34-39 (input only),
 * 0/2/15 (strapping), 16/17/4 (RS-485, above).
 *
 * These must be compile-time constants -- FastLED takes the pin as a
 * template parameter, so they cannot live in a runtime array. */
#define PIN_ROOF_DATA    32
#define PIN_FLOOR_DATA   23

static const int PIN_RS485_RX    = 16;
static const int PIN_RS485_TX    = 17;
static const int PIN_RS485_DE_RE = 4;

/* ---- Modbus config ---- */
static const uint8_t  SLAVE_ID   = 0x20;
static const uint32_t RS485_BAUD = 115200;

/* Must absorb everything that can arrive while FastLED.show() blocks
 * the loop -- see the note at Serial2.setRxBufferSize(). */
#define RS485_RX_BUF_BYTES 2048

/* ---- LED buffer ceiling (RAM only -- actual driven count is
 * runtime-configurable, see LED COUNT section above).
 *
 * 1000 per zone = 2000 pixels total, 6 KB of CRGB buffers -- trivial
 * against the ESP32's 320 KB. The real costs of going this long are
 * NOT memory:
 *
 *   Refresh: WS2812B clocks out at ~30 us/pixel, so 1000 pixels is
 *   ~30 ms per show(). Both zones stream in parallel over RMT, so it
 *   is ~30 ms total, not 60 -- but that is still 30 ms during which
 *   show() blocks. See RS485_RX_BUF_BYTES below for why that matters.
 *
 *   Power: 2000 pixels at full white is on the order of 100 A. Real
 *   scenes draw far less, but the supply and injection points must be
 *   sized for the strip, not for the controller. Inject every 1-2 m;
 *   a single feed at one end will brown out badly down the run. */
#define MAX_LEDS_PER_ZONE  1000
#define DEFAULT_LED_COUNT  30
#define LED_CHIPSET        WS2812B

/* Channel order of the fitted strip -- NOT the same on every batch.
 * Textbook WS2812B is GRB, but the strips used here are BRG: with GRB
 * set, picking blue lit green and picking green lit blue, while red and
 * cyan looked correct (a green/blue swap leaves those two untouched).
 *
 * If a future batch shows wrong colours again, this is the only line to
 * change. Diagnose it by driving pure red, then pure green, then pure
 * blue and noting what actually lights: the letter positions are the
 * order the strip expects its bytes in. */
#define LED_COLOR_ORDER    BRG

/* ---- Per-zone register layout (unchanged from the PWM version) ---- */
enum {
    ZREG_R          = 0,
    ZREG_G          = 1,
    ZREG_B          = 2,
    ZREG_MODE       = 3,
    ZREG_SPEED      = 4,
    ZREG_BRIGHTNESS = 5,
    ZREG_COUNT      = 6,
};

enum {
    MODE_STATIC  = 0,
    MODE_RAINBOW = 1,
    MODE_OFF     = 2,
};

enum { ZONE_ROOF = 0, ZONE_FLOOR = 1, ZONE_N = 2 };

/* ---- Strip-length registers (12, 13) ----
 * These live AFTER the two 6-register zone blocks, so the zone layout
 * above is untouched and the HMI's existing colour writes still work
 * byte-for-byte. The installer sets these from the HMI's Maintenance
 * screen once, when the strip is cut to fit the vehicle; the slave
 * persists whatever it receives to flash, so the count survives an HMI
 * swap, a power cycle, or the HMI simply being switched off.
 *
 * NOTE these hold 0..MAX_LEDS_PER_ZONE (up to 300), NOT 0..255 like
 * the colour registers -- on_hreg_set() must special-case them or the
 * byte clamp would silently cap a 5 m strip at 255 LEDs. */
enum {
    HREG_ROOF_COUNT  = ZREG_COUNT * ZONE_N,        /* 12 */
    HREG_FLOOR_COUNT = ZREG_COUNT * ZONE_N + 1,    /* 13 */
    HREG_TOTAL       = ZREG_COUNT * ZONE_N + 2,    /* 14 registers total */
};

ModbusRTU mb;
Preferences prefs;

typedef struct {
    const char *name;
    uint16_t    base_reg;         /* first holding register for this zone */
    CRGB       *leds;             /* points into the static buffers below  */
    uint16_t    count;            /* active LED count, runtime-configurable */
    const char *nvs_key;          /* Preferences key for persisted count   */
    uint16_t    count_reg;        /* holding register carrying that count  */

    /* Rainbow animation state (per-zone so both zones can cycle
     * independently and at independent speeds). */
    uint32_t    last_step_ms;
    uint8_t     rainbow_hue;

    /* Change detection, so we only clock pixels out when needed. */
    uint32_t    last_sig;
    bool        drawn_once;
} rgb_zone_t;

static CRGB s_buf_roof [MAX_LEDS_PER_ZONE];
static CRGB s_buf_floor[MAX_LEDS_PER_ZONE];

static rgb_zone_t s_zones[ZONE_N] = {
    { "ROOF",  0,          s_buf_roof,  DEFAULT_LED_COUNT, "cnt_roof",
      HREG_ROOF_COUNT,  0, 0, 0, false },
    { "FLOOR", ZREG_COUNT, s_buf_floor, DEFAULT_LED_COUNT, "cnt_floor",
      HREG_FLOOR_COUNT, 0, 0, 0, false },
};


/* ---- Modbus callbacks ------------------------------------------------ */

/* Shared clamp / validation for every writable holding register.
 * Called by the library on each FC 0x06 / FC 0x10 register update. */
static uint16_t on_hreg_set(TRegister *r, uint16_t value) {
    uint16_t addr = r->address.address;

    /* Strip-length registers first: they are NOT part of the 6-register
     * zone blocks, so the `addr % ZREG_COUNT` mapping below would
     * misread reg 12 as ZREG_R and clamp a 300-LED strip down to 255. */
    if (addr >= HREG_ROOF_COUNT) {
        if (value > MAX_LEDS_PER_ZONE) value = MAX_LEDS_PER_ZONE;
        return value;
    }

    uint16_t off = addr % ZREG_COUNT;    /* per-zone offset */

    switch (off) {
        case ZREG_R:
        case ZREG_G:
        case ZREG_B:
        case ZREG_BRIGHTNESS:
            if (value > 255) value = 255;
            break;
        case ZREG_MODE:
            if (value > MODE_OFF) value = MODE_OFF;
            break;
        case ZREG_SPEED:
            if (value < 1)   value = 1;
            if (value > 255) value = 255;
            break;
        default:
            break;
    }
    return value;
}


/* ---- Per-zone tick --------------------------------------------------- */

/* Recompute this zone's pixels. Returns true only if the result differs
 * from what is already latched in the strip.
 *
 * This matters for bus reliability, not just efficiency: FastLED.show()
 * blocks while it clocks the pixels out, and calling it on every loop
 * iteration (a few hundred times a second) steals time from mb.task(),
 * which has to service Modbus RTU framing. In STATIC mode -- what the
 * strip is in almost all the time -- nothing changes between ticks, so
 * there is no reason to push at all. */
static bool zone_tick(rgb_zone_t *z) {
    uint8_t mode  = (uint8_t)mb.Hreg(z->base_reg + ZREG_MODE);
    uint8_t bri   = (uint8_t)mb.Hreg(z->base_reg + ZREG_BRIGHTNESS);
    uint8_t speed = (uint8_t)mb.Hreg(z->base_reg + ZREG_SPEED);
    if (speed < 1) speed = 1;

    /* Signature of everything that affects the output. Cheap to compare
     * against the last frame; avoids memcmp'ing the whole pixel buffer. */
    uint32_t sig = ((uint32_t)mode << 24) ^ ((uint32_t)bri << 16) ^ z->count;
    if (mode == MODE_STATIC) {
        sig ^= ((uint32_t)(uint8_t)mb.Hreg(z->base_reg + ZREG_R) << 16)
             ^ ((uint32_t)(uint8_t)mb.Hreg(z->base_reg + ZREG_G) << 8)
             ^  (uint32_t)(uint8_t)mb.Hreg(z->base_reg + ZREG_B);
    }

    bool sig_changed = !z->drawn_once || sig != z->last_sig;

    if (mode == MODE_RAINBOW) {
        /* Animating: redraw when the animation steps -- but also
         * immediately on a settings change, so switching INTO rainbow
         * (or changing brightness mid-animation) isn't held back until
         * the next step, which at SPEED=255 would be a quarter second. */
        bool stepped = (uint32_t)(millis() - z->last_step_ms) >= speed;
        if (!sig_changed && !stepped) return false;
    } else if (!sig_changed) {
        return false;
    }
    z->last_sig   = sig;
    z->drawn_once = true;

    switch (mode) {
        case MODE_STATIC: {
            uint8_t r = (uint8_t)mb.Hreg(z->base_reg + ZREG_R);
            uint8_t g = (uint8_t)mb.Hreg(z->base_reg + ZREG_G);
            uint8_t b = (uint8_t)mb.Hreg(z->base_reg + ZREG_B);
            fill_solid(z->leds, z->count, CRGB(r, g, b));
            break;
        }

        case MODE_RAINBOW: {
            uint32_t now = millis();
            if ((uint32_t)(now - z->last_step_ms) >= speed) {
                z->last_step_ms = now;
                z->rainbow_hue++;
            }
            /* One full rainbow cycle spread across whatever the strip's
             * current length is, so it always looks right after you
             * change the LED count -- no retuning needed. */
            uint8_t delta_hue = (z->count > 0) ? (255 / z->count) : 0;
            if (delta_hue < 1) delta_hue = 1;
            fill_rainbow(z->leds, z->count, z->rainbow_hue, delta_hue);
            break;
        }

        case MODE_OFF:
        default:
            fill_solid(z->leds, z->count, CRGB::Black);
            break;
    }

    /* Brightness is per-zone, so it can't use FastLED's single global
     * FastLED.setBrightness() (that applies to every zone at once).
     * Scale each pixel here instead -- same approach the old PWM
     * version used with scale8(). */
    if (bri != 255) {
        for (uint16_t i = 0; i < z->count; i++) {
            z->leds[i].nscale8(bri);
        }
    }
    return true;
}


/* ---- Runtime LED-count configuration (Serial CLI + flash persist) ---- */

static void led_count_set(rgb_zone_t *z, long n) {
    if (n < 0) n = 0;
    if (n > MAX_LEDS_PER_ZONE) n = MAX_LEDS_PER_ZONE;
    if ((uint16_t)n == z->count) return;   /* no-op: don't churn flash */

    z->count = (uint16_t)n;

    /* Clear the whole compiled buffer, not just the active count, so
     * shrinking a strip doesn't leave stale lit pixels beyond the new
     * end (they're unused but harmless; this just keeps state tidy). */
    fill_solid(z->leds, MAX_LEDS_PER_ZONE, CRGB::Black);

    prefs.putUShort(z->nvs_key, z->count);
    /* Publish into the holding register so the HMI reads back the truth
     * on its next FC 0x03, and so enforce_led_counts() sees no drift. */
    mb.Hreg(z->count_reg, z->count);

    Serial.print(F("[rgb-slave] "));
    Serial.print(z->name);
    Serial.print(F(" LED count set to "));
    Serial.print(z->count);
    Serial.println(F(" (saved)"));
}

/* Keep the strip-length registers pinned to what THIS board knows.
 *
 * The slave is authoritative for strip length: it is the device
 * physically wired to the strip, so it is the only one that can know
 * how long the strip is. The HMI may READ regs 12/13 to display the
 * value, but a write from the bus is rejected -- otherwise anyone
 * opening the HMI's LED Strips screen and pressing Save would push
 * that screen's own (lower) limit over the real length and truncate
 * the strip.
 *
 * Change the length here, on the board, with ROOF/FLOOR over serial. */
static void enforce_led_counts(void) {
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        uint16_t reg = mb.Hreg(z->count_reg);
        if (reg != z->count) {
            /* Something on the bus wrote a different length. Reject it
             * and put the real value back, so the HMI also reads the
             * truth on its next FC 0x03. */
            Serial.print(F("[rgb-slave] ignored bus write of "));
            Serial.print(reg);
            Serial.print(F(" to "));
            Serial.print(z->name);
            Serial.print(F(" count; keeping "));
            Serial.println(z->count);
            mb.Hreg(z->count_reg, z->count);
        }
    }
}

static void print_counts(void) {
    for (int i = 0; i < ZONE_N; i++) {
        Serial.print(F("  "));
        Serial.print(s_zones[i].name);
        Serial.print(F(": "));
        Serial.print(s_zones[i].count);
        Serial.println(F(" LEDs"));
    }
}

/* Stringify MAX_LEDS_PER_ZONE for the help text. Two levels so the
 * macro is expanded to its value before being turned into a string. */
#define STR2(x) #x
#define STR(x)  STR2(x)

static void print_help(void) {
    Serial.println(F("[rgb-slave] commands:"));
    Serial.println(F("  ROOF <n>   -- set roof strip LED count (0.." STR(MAX_LEDS_PER_ZONE) ")"));
    Serial.println(F("  FLOOR <n>  -- set floor strip LED count"));
    Serial.println(F("  COUNTS     -- print current LED counts"));
}

static void serial_cli_poll(void) {
    static char line[32];
    static uint8_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0';
            len = 0;

            if (strncasecmp(line, "ROOF ", 5) == 0) {
                led_count_set(&s_zones[ZONE_ROOF], atol(line + 5));
            } else if (strncasecmp(line, "FLOOR ", 6) == 0) {
                led_count_set(&s_zones[ZONE_FLOOR], atol(line + 6));
            } else if (strcasecmp(line, "COUNTS") == 0) {
                print_counts();
            } else if (line[0] != '\0') {
                print_help();
            }
            continue;
        }
        if (len < sizeof(line) - 1) line[len++] = c;
    }
}


/* ---- Arduino entry points -------------------------------------------- */

void setup(void) {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println(F("[rgb-slave] boot (2 zones, addressable LEDs)"));

    /* Load persisted LED counts (defaults to DEFAULT_LED_COUNT on a
     * fresh chip / first boot). */
    prefs.begin("rgbslave", false);
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        z->count = prefs.getUShort(z->nvs_key, DEFAULT_LED_COUNT);
        if (z->count > MAX_LEDS_PER_ZONE) z->count = MAX_LEDS_PER_ZONE;
    }

    /* FastLED controllers, one per zone. Buffers are the full compiled
     * ceiling; zone_tick() only touches the active `count` pixels. */
    /* Three controllers per zone, all pointed at that zone's ONE buffer.
     * FastLED is happy to share a buffer between controllers, so the
     * runs stay in lockstep for free -- there is nothing to keep in
     * sync, they are literally the same pixels.
     *
     * The ESP32 has 8 RMT channels and these 6 outputs take one each,
     * driven concurrently -- so show() costs about the same as a single
     * strip, not six times as much. */
    FastLED.addLeds<LED_CHIPSET, PIN_ROOF_DATA,  LED_COLOR_ORDER>(s_buf_roof,  MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_FLOOR_DATA, LED_COLOR_ORDER>(s_buf_floor, MAX_LEDS_PER_ZONE);
    fill_solid(s_buf_roof,  MAX_LEDS_PER_ZONE, CRGB::Black);
    fill_solid(s_buf_floor, MAX_LEDS_PER_ZONE, CRGB::Black);
    FastLED.show();

    /* RS-485 UART. */
    /* Enlarge the RX FIFO before opening the port.
     *
     * FastLED.show() blocks while it clocks pixels out -- ~30 ms for a
     * 1000-pixel chain. At 115200 baud that is room for ~345 bytes of
     * Modbus traffic to arrive with nothing servicing the UART, and the
     * default 256-byte buffer would overrun and drop frames from the
     * HMI. 2 KB gives comfortable headroom even if a future strip is
     * longer still. Must be called BEFORE begin() to take effect. */
    Serial2.setRxBufferSize(RS485_RX_BUF_BYTES);
    Serial2.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
    mb.begin(&Serial2, PIN_RS485_DE_RE);
    mb.setBaudrate(RS485_BAUD);
    mb.slave(SLAVE_ID);

    /* Register all holding registers: 6 per zone + 2 strip lengths. */
    const uint16_t total = HREG_TOTAL;
    for (uint16_t i = 0; i < total; i++) {
        mb.addHreg(i, 0);
    }
    /* Sensible per-zone defaults: speed=20 (match original animation),
     * brightness=255 (full). Seed the strip-length registers from the
     * counts just loaded out of flash, so an HMI polling FC 0x03 reads
     * the real installed lengths rather than zeros. */
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        mb.Hreg(z->base_reg + ZREG_SPEED,      20);
        mb.Hreg(z->base_reg + ZREG_BRIGHTNESS, 255);
        mb.Hreg(z->count_reg,                  z->count);
    }

    /* One callback covers every writable register. */
    mb.onSetHreg(0, on_hreg_set, total);

    Serial.print(F("[rgb-slave] ready, addr=0x"));
    Serial.print(SLAVE_ID, HEX);
    Serial.print(F(", roof regs 0..5, floor regs "));
    Serial.print(ZREG_COUNT);
    Serial.print(F(".."));
    Serial.println(total - 1);
    Serial.println(F("[rgb-slave] LED counts (type COUNTS / ROOF n / FLOOR n to change):"));
    print_counts();
    print_help();
}

void loop(void) {
    mb.task();                 /* service RTU state machine */
    enforce_led_counts();      /* slave owns the strip length            */
    serial_cli_poll();         /* ROOF/FLOOR/COUNTS commands over USB   */

    bool dirty = false;
    for (int i = 0; i < ZONE_N; i++) {
        if (zone_tick(&s_zones[i])) dirty = true;
    }

    /* Only clock the strips when something actually changed. show()
     * blocks while it shifts every pixel out and pushes BOTH zones in
     * one call, so doing it unconditionally (a few hundred times a
     * second) would steal time from mb.task() and cost us Modbus frames
     * on a bus we share with the relay board. In STATIC mode -- the
     * normal case -- this now sends nothing at all until the master
     * writes a new colour. */
    if (dirty) FastLED.show();

    /* Service the RTU state machine again before yielding: show() may
     * have just blocked for a few ms, and a reply that arrived during
     * it is sitting in the UART FIFO. */
    if (dirty) mb.task();

    delay(1);                  /* WDT-friendly yield */
}
