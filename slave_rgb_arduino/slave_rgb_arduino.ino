/* =======================================================================
 *  Hyperwisor RGB Slave — ESP32 + MAX485 (HW-097)
 *
 *  Role on the bus : Modbus RTU SLAVE
 *  Slave address   : 0x20 (32 decimal)
 *  Bus             : RS-485, 9600 8N1, same wires as the Waveshare
 *                    relay board (0x01).
 *  Master          : ESP32-S3 HMI (hyperwisor_s3).
 *
 *  This single slave drives TWO independent RGB zones:
 *    - ROOF  : LEDs behind the head-liner
 *    - FLOOR : LEDs in the floor / footwell channel
 *
 *  Each zone has its own PWM triplet and its own mirrored block of
 *  6 holding registers. A MASTER zone record (R/G/B/MODE/SPEED/BRIGHTNESS)
 *  means you can run Roof rainbow while Floor is static red, etc.
 *
 *  Holding-register map
 *  --------------------
 *   ROOF ZONE   regs 0..5    (matches original single-zone layout)
 *     Reg 0  R          0..255
 *     Reg 1  G          0..255
 *     Reg 2  B          0..255
 *     Reg 3  MODE       0 STATIC, 1 RAINBOW, 2 OFF
 *     Reg 4  SPEED      1..255  ms between rainbow steps (lower=faster)
 *     Reg 5  BRIGHTNESS 0..255  global multiplier for this zone
 *
 *   FLOOR ZONE  regs 6..11  (same layout as above)
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
 *  Roof XY-MOS  : R=GPIO12, G=GPIO13, B=GPIO14
 *  Floor XY-MOS : R=GPIO25, G=GPIO26, B=GPIO27   (change if you're on S3)
 *
 *  Library
 *  -------
 *   emelianov / modbus-esp8266  (install as ZIP if Library Manager
 *   can't find it — works on ESP32 despite the name).
 * ======================================================================= */

#include <ModbusRTU.h>

/* ---- Pin map ---- */
/* Zone 0 = ROOF */
static const int PIN_ROOF_R  = 12;
static const int PIN_ROOF_G  = 13;
static const int PIN_ROOF_B  = 14;
/* Zone 1 = FLOOR */
static const int PIN_FLOOR_R = 25;
static const int PIN_FLOOR_G = 26;
static const int PIN_FLOOR_B = 27;

static const int PIN_RS485_RX    = 16;
static const int PIN_RS485_TX    = 17;
static const int PIN_RS485_DE_RE = 4;

/* ---- Modbus config ---- */
static const uint8_t  SLAVE_ID   = 0x20;
static const uint32_t RS485_BAUD = 9600;

/* ---- PWM config ---- */
static const int LEDC_FREQ = 5000;
static const int LEDC_RES  = 8;       /* 0..255 */

/* ---- Per-zone register layout ----
 * Each zone occupies a 6-register block. The master picks which zone
 * by starting its FC 0x10 write at zone->base_reg. */
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

ModbusRTU mb;

typedef struct {
    const char *name;
    uint8_t     pin_r, pin_g, pin_b;
    uint16_t    base_reg;       /* first holding register for this zone */

    /* Cache of last values pushed to PWM so we skip redundant writes. */
    uint8_t     last_r, last_g, last_b;

    /* Rainbow animation state (per-zone so both zones can cycle
     * independently and at independent speeds). */
    uint32_t    last_step_ms;
    uint8_t     rainbow_hue;
} rgb_zone_t;

static rgb_zone_t s_zones[ZONE_N] = {
    { "ROOF",  PIN_ROOF_R,  PIN_ROOF_G,  PIN_ROOF_B,
      /* base_reg = */ 0, 0xFF, 0xFF, 0xFF, 0, 0 },
    { "FLOOR", PIN_FLOOR_R, PIN_FLOOR_G, PIN_FLOOR_B,
      /* base_reg = */ ZREG_COUNT, 0xFF, 0xFF, 0xFF, 0, 0 },
};


/* ---- RGB pipeline ---------------------------------------------------- */

static inline uint8_t scale8(uint8_t v, uint8_t scale) {
    return (uint16_t)v * (uint16_t)scale / 255u;
}

static void zone_write_rgb(rgb_zone_t *z, uint8_t r, uint8_t g, uint8_t b) {
    if (r != z->last_r) { ledcWrite(z->pin_r, r); z->last_r = r; }
    if (g != z->last_g) { ledcWrite(z->pin_g, g); z->last_g = g; }
    if (b != z->last_b) { ledcWrite(z->pin_b, b); z->last_b = b; }
}

/* Verbatim color wheel from the original standalone sketch. */
static void color_wheel(uint8_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (pos < 85) {
        *r = 255 - pos * 3;
        *g = pos * 3;
        *b = 0;
    } else if (pos < 170) {
        pos -= 85;
        *r = 0;
        *g = 255 - pos * 3;
        *b = pos * 3;
    } else {
        pos -= 170;
        *r = pos * 3;
        *g = 0;
        *b = 255 - pos * 3;
    }
}


/* ---- Modbus callbacks ------------------------------------------------ */

/* Shared clamp / validation for every writable holding register.
 * Called by the library on each FC 0x06 / FC 0x10 register update. */
static uint16_t on_hreg_set(TRegister *r, uint16_t value) {
    uint16_t addr = r->address.address;
    uint16_t off  = addr % ZREG_COUNT;   /* per-zone offset */

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

static void zone_tick(rgb_zone_t *z) {
    uint8_t mode  = (uint8_t)mb.Hreg(z->base_reg + ZREG_MODE);
    uint8_t bri   = (uint8_t)mb.Hreg(z->base_reg + ZREG_BRIGHTNESS);
    uint8_t speed = (uint8_t)mb.Hreg(z->base_reg + ZREG_SPEED);
    if (speed < 1) speed = 1;

    uint8_t r = 0, g = 0, b = 0;

    switch (mode) {
        case MODE_STATIC:
            r = (uint8_t)mb.Hreg(z->base_reg + ZREG_R);
            g = (uint8_t)mb.Hreg(z->base_reg + ZREG_G);
            b = (uint8_t)mb.Hreg(z->base_reg + ZREG_B);
            break;

        case MODE_RAINBOW: {
            uint32_t now = millis();
            if ((uint32_t)(now - z->last_step_ms) >= speed) {
                z->last_step_ms = now;
                z->rainbow_hue++;
            }
            color_wheel(z->rainbow_hue, &r, &g, &b);
            break;
        }

        case MODE_OFF:
        default:
            r = g = b = 0;
            break;
    }

    if (bri != 255) {
        r = scale8(r, bri);
        g = scale8(g, bri);
        b = scale8(b, bri);
    }

    zone_write_rgb(z, r, g, b);
}


/* ---- Arduino entry points -------------------------------------------- */

void setup(void) {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println(F("[rgb-slave] boot (2 zones)"));

    /* PWM channels for both zones. */
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        ledcAttach(z->pin_r, LEDC_FREQ, LEDC_RES);
        ledcAttach(z->pin_g, LEDC_FREQ, LEDC_RES);
        ledcAttach(z->pin_b, LEDC_FREQ, LEDC_RES);
        zone_write_rgb(z, 0, 0, 0);
    }

    /* RS-485 UART. */
    Serial2.begin(RS485_BAUD, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
    mb.begin(&Serial2, PIN_RS485_DE_RE);
    mb.setBaudrate(RS485_BAUD);
    mb.slave(SLAVE_ID);

    /* Register the 12 holding registers (6 per zone). */
    const uint16_t total = ZREG_COUNT * ZONE_N;
    for (uint16_t i = 0; i < total; i++) {
        mb.addHreg(i, 0);
    }
    /* Sensible per-zone defaults: speed=20 (match original animation),
     * brightness=255 (full). */
    for (int i = 0; i < ZONE_N; i++) {
        rgb_zone_t *z = &s_zones[i];
        mb.Hreg(z->base_reg + ZREG_SPEED,      20);
        mb.Hreg(z->base_reg + ZREG_BRIGHTNESS, 255);
    }

    /* One callback covers every writable register. */
    mb.onSetHreg(0, on_hreg_set, total);

    Serial.print(F("[rgb-slave] ready, addr=0x"));
    Serial.print(SLAVE_ID, HEX);
    Serial.print(F(", roof regs 0..5, floor regs "));
    Serial.print(ZREG_COUNT);
    Serial.print(F(".."));
    Serial.println(total - 1);
}

void loop(void) {
    mb.task();                 /* service RTU state machine */

    for (int i = 0; i < ZONE_N; i++) {
        zone_tick(&s_zones[i]);
    }

    delay(1);                  /* WDT-friendly yield */
}
