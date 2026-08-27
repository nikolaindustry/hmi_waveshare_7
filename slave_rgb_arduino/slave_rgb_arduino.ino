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
 *  LED COUNT — set at runtime, no reflash needed
 *  -----------------------------------------------
 *  How many LEDs are actually wired isn't known until the strip is
 *  cut to fit the vehicle, so it is NOT a compile-time constant.
 *  Two ways to set it, and both persist to flash (Preferences/NVS):
 *
 *   1. FROM THE HMI (normal path). The installer sets the length on
 *      the touchscreen while fitting the strip — no laptop needed.
 *      The HMI writes regs 12/13; this slave applies and saves them.
 *      Storage lives HERE, on the board wired to the strip, so the
 *      count survives an HMI swap or the HMI being switched off.
 *
 *   2. FROM SERIAL (bench / debug fallback). Open the Arduino Serial
 *      Monitor at 115200 baud and type:
 *
 *          ROOF 45      <Enter>   -- set roof run to 45 LEDs
 *          FLOOR 60     <Enter>   -- set floor run to 60 LEDs
 *          COUNTS       <Enter>   -- print current counts
 *
 *  NOTE the count is the length of ONE run. Each zone has three
 *  mirrored runs (see wiring), so a roof count of 300 lights 900 LEDs.
 *
 *  Either path updates regs 12/13, so the HMI always reads back the
 *  truth via FC 0x03. MAX_LEDS_PER_ZONE below is just the compiled
 *  buffer's ceiling (RAM), not the active count — set generously
 *  (300 = 5 m of 60/m strip) so you never hit it in practice.
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
 *     ESP32           LED strips -- 3 MIRRORED runs per zone
 *     GPIO 13  ──►    ROOF  run 1 DATA-IN
 *     GPIO 14  ──►    ROOF  run 2 DATA-IN
 *     GPIO 27  ──►    ROOF  run 3 DATA-IN
 *     GPIO 25  ──►    FLOOR run 1 DATA-IN
 *     GPIO 26  ──►    FLOOR run 2 DATA-IN
 *     GPIO 33  ──►    FLOOR run 3 DATA-IN
 *
 *     All three runs in a zone show the SAME thing, so a zone can be
 *     3 x <count> LEDs long in total. Set <count> to the length of ONE
 *     run, not the total.
 *     5V/GND   ──►    strip power (size the PSU for LED count x ~60 mA
 *                     peak per pixel at full white; add a 300-500 ohm
 *                     resistor in series with each DATA line and a
 *                     large (>=1000 uF) cap across strip 5V/GND at the
 *                     strip's input end -- standard WS2812B practice)
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

/* ---- LED data pins: 3 MIRRORED outputs per zone ----
 *
 * Each zone drives three separate physical runs that all receive the
 * SAME data, so one zone can light 3 x <count> LEDs while the Modbus
 * protocol and the HMI still deal with a single count (the length of
 * ONE run). Splitting the load this way also keeps each chain short,
 * which means a faster refresh and far less voltage droop than one long
 * daisy-chain would have.
 *
 * Because the runs are mirrors, they show the SAME thing -- three
 * identical rainbows rather than one rainbow spread over all of them.
 * For static colour, which is the normal case, they are indivisible.
 *
 * NOT GPIO12: that is the MTDI strapping pin, sampled at reset to pick
 * the flash voltage. If anything on the strip side (a level shifter's
 * pull-up, a long floating lead) holds it high at power-on the module
 * won't boot. Also avoided: 6-11 (SPI flash), 34-39 (input only),
 * 0/2/15 (strapping), 16/17/4 (RS-485, above).
 *
 * These must be compile-time constants -- FastLED takes the pin as a
 * template parameter, so they cannot live in a runtime array. */
#define PIN_ROOF_A     13
#define PIN_ROOF_B     14
#define PIN_ROOF_C     27
#define PIN_FLOOR_A    25
#define PIN_FLOOR_B    26
#define PIN_FLOOR_C    33

static const int PIN_RS485_RX    = 16;
static const int PIN_RS485_TX    = 17;
static const int PIN_RS485_DE_RE = 4;

/* ---- Modbus config ---- */
static const uint8_t  SLAVE_ID   = 0x20;
static const uint32_t RS485_BAUD = 115200;

/* ---- LED buffer ceiling (RAM only -- actual driven count is
 * runtime-configurable, see LED COUNT section above). ---- */
#define MAX_LEDS_PER_ZONE  300
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
    /* Mirror into the holding register so the HMI reads back the truth
     * (FC 0x03) whichever way the count was changed -- touchscreen or
     * the serial CLI below. Also keeps sync_led_counts() a no-op. */
    mb.Hreg(z->count_reg, z->count);

    Serial.print(F("[rgb-slave] "));
    Serial.print(z->name);
    Serial.print(F(" LED count set to "));
    Serial.print(z->count);
    Serial.println(F(" (saved)"));
}

/* Poll the strip-length registers for changes pushed by the HMI.
 *
 * Done here rather than inside on_hreg_set() because that callback
 * fires on EVERY write -- including the HMI re-sending an unchanged
 * value -- and writing flash each time would wear out NVS. Comparing
 * against the live count means flash is only touched when the number
 * actually changes. */
static void sync_led_counts(void) {
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        uint16_t want = mb.Hreg(z->count_reg);
        if (want != z->count) {
            Serial.print(F("[rgb-slave] HMI set "));
            Serial.print(z->name);
            Serial.print(F(" -> "));
            Serial.println(want);
            led_count_set(z, want);
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
    FastLED.addLeds<LED_CHIPSET, PIN_ROOF_A,  LED_COLOR_ORDER>(s_buf_roof,  MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_ROOF_B,  LED_COLOR_ORDER>(s_buf_roof,  MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_ROOF_C,  LED_COLOR_ORDER>(s_buf_roof,  MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_FLOOR_A, LED_COLOR_ORDER>(s_buf_floor, MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_FLOOR_B, LED_COLOR_ORDER>(s_buf_floor, MAX_LEDS_PER_ZONE);
    FastLED.addLeds<LED_CHIPSET, PIN_FLOOR_C, LED_COLOR_ORDER>(s_buf_floor, MAX_LEDS_PER_ZONE);
    fill_solid(s_buf_roof,  MAX_LEDS_PER_ZONE, CRGB::Black);
    fill_solid(s_buf_floor, MAX_LEDS_PER_ZONE, CRGB::Black);
    FastLED.show();

    /* RS-485 UART. */
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
    sync_led_counts();         /* strip lengths pushed from the HMI     */
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
