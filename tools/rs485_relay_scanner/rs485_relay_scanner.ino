/* =======================================================================
 *  Waveshare Modbus RTU Relay — bus scanner / baud + address recovery
 *
 *  For when the relay board has "gone silent": its baud rate and/or
 *  slave address are unknown, so nothing on the bus can talk to it.
 *  This sweeps every plausible baud rate and address and reports what
 *  answers.
 *
 *  Flash to an ESP32 wired to the RS-485 bus, open the Serial Monitor
 *  at 115200, and read the report. Nothing is written to the relay --
 *  this is read-only until you explicitly run the FIX command.
 *
 *  Wiring (same as the RGB slave board, so that harness can be reused)
 *  ------------------------------------------------------------------
 *     ESP32          MAX485 / HW-097
 *     GPIO 17  ──►   DI          (data out from ESP32)
 *     GPIO 16  ◄──   RO          (data in to ESP32)
 *     GPIO 4   ──►   DE + RE tied together
 *     3V3/5V   ──►   VCC
 *     GND      ──►   GND   ← must be common with the relay board
 *     A / B    ──►   the relay's A / B
 *
 *  Reading the result
 *  ------------------
 *   FOUND ...........  baud + address identified. Done.
 *   Garbage frames ..  bytes came back but the CRC never checked out.
 *                      Wiring is fine and something IS transmitting --
 *                      almost certainly still a baud mismatch, or two
 *                      devices answering at once. Note which baud gave
 *                      the cleanest-looking data.
 *   Total silence ...  nothing at ANY baud. That is not a baud problem:
 *                      suspect A/B swapped, no common ground, relay
 *                      unpowered, DE/RE pin wrong, or the 120R
 *                      termination loading the bus down. Swapping A and
 *                      B is the single most common fix and is harmless
 *                      to try.
 * ======================================================================= */

/* ---- wiring ---- */
#define PIN_RS485_RX    16
#define PIN_RS485_TX    17
#define PIN_RS485_DE_RE 4

/* ---- what to sweep ----
 * Ordered most-likely-first so a common setup is found in seconds.
 * 9600 is the Waveshare factory default; 115200 is what this project
 * reconfigures boards to. */
static const uint32_t BAUDS[] = {
    9600, 115200, 19200, 38400, 57600, 4800, 2400, 1200
};
static const uint8_t N_BAUDS = sizeof(BAUDS) / sizeof(BAUDS[0]);

/* Waveshare boards are 8N1, but a mis-set board (or a different relay
 * entirely) may not be -- so parity is swept too, cheaply, and only
 * after 8N1 has failed everywhere. */
static const uint32_t FRAMINGS[]      = { SERIAL_8N1, SERIAL_8E1, SERIAL_8O1 };
static const char    *FRAMING_NAMES[] = { "8N1",      "8E1",      "8O1"      };
static const uint8_t  N_FRAMINGS = 3;

/* Bus addresses are user-settable. 1..32 covers any sane install and
 * keeps the sweep quick; SCAN_ADDR_MAX can go to 247 for a full sweep. */
#define SCAN_ADDR_MIN   1
#define SCAN_ADDR_MAX   32

/* Waveshare relay config registers (from the board datasheet). */
#define WS_REG_BAUD     0x2000   /* 2 = 9600, 5 = 115200 */
#define WS_REG_ADDR     0x4000   /* device address                     */

#define RESP_TIMEOUT_MS 120      /* generous: 1200 baud replies crawl  */

/* ---- results ---- */
static bool     s_found;
static uint32_t s_found_baud;
static uint8_t  s_found_addr;
static uint32_t s_found_framing;
static const char *s_found_framing_name;
static uint32_t s_garbage_seen;   /* frames received but CRC-invalid   */
static uint32_t s_bytes_seen;     /* any bytes at all, ever            */


/* ---- Modbus helpers --------------------------------------------------- */

static uint16_t crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

/* Send a frame (CRC appended here) and collect whatever comes back.
 * Returns the byte count received. */
static size_t txrx(const uint8_t *req, size_t req_len,
                   uint8_t *resp, size_t resp_cap) {
    uint8_t frame[16];
    if (req_len + 2 > sizeof(frame)) return 0;
    memcpy(frame, req, req_len);
    uint16_t c = crc16(frame, req_len);
    frame[req_len]     = c & 0xFF;         /* CRC is little-endian */
    frame[req_len + 1] = (c >> 8) & 0xFF;

    while (Serial2.available()) Serial2.read();   /* drop stale bytes */

    digitalWrite(PIN_RS485_DE_RE, HIGH);          /* drive the bus    */
    Serial2.write(frame, req_len + 2);
    Serial2.flush();                              /* wait for last stop bit */
    digitalWrite(PIN_RS485_DE_RE, LOW);           /* release to listen */

    size_t n = 0;
    uint32_t deadline = millis() + RESP_TIMEOUT_MS;
    while (millis() < deadline && n < resp_cap) {
        if (Serial2.available()) {
            resp[n++] = Serial2.read();
            /* Extend slightly on each byte so a slow reply isn't cut
             * in half and misreported as garbage. */
            deadline = millis() + 20;
        }
    }
    return n;
}

static bool crc_ok(const uint8_t *buf, size_t n) {
    if (n < 4) return false;
    uint16_t want = crc16(buf, n - 2);
    uint16_t got  = (uint16_t)buf[n - 2] | ((uint16_t)buf[n - 1] << 8);
    return want == got;
}

static void print_hex(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (b[i] < 0x10) Serial.print('0');
        Serial.print(b[i], HEX);
        Serial.print(' ');
    }
}


/* ---- probes ----------------------------------------------------------- */

/* Ask one address to read its own baud register. Any CRC-valid reply
 * whose address byte matches means we have found the board. */
static bool probe_addr(uint8_t addr) {
    uint8_t req[6] = { addr, 0x03,
                       (WS_REG_BAUD >> 8) & 0xFF, WS_REG_BAUD & 0xFF,
                       0x00, 0x01 };
    uint8_t resp[32];
    size_t n = txrx(req, sizeof(req), resp, sizeof(resp));
    if (n == 0) return false;

    s_bytes_seen += n;
    if (!crc_ok(resp, n) || resp[0] != addr) {
        s_garbage_seen++;
        return false;
    }
    Serial.print(F("\n  *** reply from addr "));
    Serial.print(addr);
    Serial.print(F(": "));
    print_hex(resp, n);
    Serial.println();
    return true;
}

/* Waveshare exposes a broadcast read of the address register: send to
 * address 0x00 and the board answers with its own address. That finds
 * the address in ONE exchange instead of sweeping 32 of them -- so it
 * is tried first at each baud. */
static bool probe_broadcast_addr(uint8_t *out_addr) {
    uint8_t req[6] = { 0x00, 0x03,
                       (WS_REG_ADDR >> 8) & 0xFF, WS_REG_ADDR & 0xFF,
                       0x00, 0x01 };
    uint8_t resp[32];
    size_t n = txrx(req, sizeof(req), resp, sizeof(resp));
    if (n == 0) return false;

    s_bytes_seen += n;
    if (!crc_ok(resp, n)) { s_garbage_seen++; return false; }

    /* Expect: <addr> 03 02 <hi> <lo> <crc16> */
    if (n >= 5 && resp[1] == 0x03) {
        *out_addr = resp[4] ? resp[4] : resp[0];
        Serial.print(F("\n  *** broadcast address query answered: "));
        print_hex(resp, n);
        Serial.println();
        return true;
    }
    return false;
}

static void open_bus(uint32_t baud, uint32_t framing) {
    Serial2.end();
    delay(20);
    Serial2.begin(baud, framing, PIN_RS485_RX, PIN_RS485_TX);
    delay(40);          /* let the UART settle before the first frame */
}

static bool sweep(uint32_t framing, const char *framing_name) {
    for (uint8_t bi = 0; bi < N_BAUDS; bi++) {
        uint32_t baud = BAUDS[bi];
        Serial.print(F("\n["));
        Serial.print(baud);
        Serial.print(F(" "));
        Serial.print(framing_name);
        Serial.print(F("] "));

        open_bus(baud, framing);

        /* Fast path: ask the bus who is there. */
        uint8_t addr = 0;
        if (probe_broadcast_addr(&addr) && addr >= 1 && addr <= 247) {
            /* Confirm with a direct unicast -- a broadcast reply alone
             * could be a fluke of a mis-framed byte stream. */
            if (probe_addr(addr)) {
                s_found = true; s_found_baud = baud; s_found_addr = addr;
                s_found_framing = framing; s_found_framing_name = framing_name;
                return true;
            }
        }

        /* Slow path: walk the address range. */
        for (uint8_t a = SCAN_ADDR_MIN; a <= SCAN_ADDR_MAX; a++) {
            if (probe_addr(a)) {
                s_found = true; s_found_baud = baud; s_found_addr = a;
                s_found_framing = framing; s_found_framing_name = framing_name;
                return true;
            }
            if ((a % 8) == 0) Serial.print('.');   /* progress */
        }
    }
    return false;
}


/* ---- report ----------------------------------------------------------- */

static void report(void) {
    Serial.println(F("\n\n================ RESULT ================"));

    if (s_found) {
        Serial.println(F("FOUND the relay board."));
        Serial.print(F("  baud    : ")); Serial.println(s_found_baud);
        Serial.print(F("  framing : ")); Serial.println(s_found_framing_name);
        Serial.print(F("  address : ")); Serial.print(s_found_addr);
        Serial.print(F("  (0x"));        Serial.print(s_found_addr, HEX);
        Serial.println(F(")"));
        Serial.println();
        Serial.println(F("Next: set the HMI to this baud"));
        Serial.println(F("  (Maintenance -> Bus Setup), or type"));
        Serial.println(F("  FIX9600 / FIX115200 here to reprogram the"));
        Serial.println(F("  relay to a standard rate."));
        if (s_found_addr != 1) {
            Serial.println();
            Serial.println(F("NOTE: address is not 1. The HMI polls slave 1"));
            Serial.println(F("      for relays -- either set the board back"));
            Serial.println(F("      to 1, or update the firmware to match."));
        }
        return;
    }

    Serial.println(F("NOT found at any baud/framing."));
    Serial.print(F("  bytes ever received : ")); Serial.println(s_bytes_seen);
    Serial.print(F("  CRC-invalid frames  : ")); Serial.println(s_garbage_seen);
    Serial.println();

    if (s_bytes_seen == 0) {
        Serial.println(F("TOTAL SILENCE -- nothing came back, ever."));
        Serial.println(F("This is NOT a baud problem. Check, in order:"));
        Serial.println(F("  1. A and B swapped  <- most common; just swap them"));
        Serial.println(F("  2. GND not shared between ESP32 and relay board"));
        Serial.println(F("  3. Relay board unpowered"));
        Serial.println(F("  4. DE/RE not on GPIO 4, or RO/DI swapped (16/17)"));
        Serial.println(F("  5. Too many 120R terminators -- only the two"));
        Serial.println(F("     devices at the physical ends should have one"));
    } else {
        Serial.println(F("Bytes DID come back but never with a valid CRC."));
        Serial.println(F("Wiring is good and something is transmitting."));
        Serial.println(F("  - Most likely still a baud mismatch (try the"));
        Serial.println(F("    non-standard rates, or widen BAUDS[])"));
        Serial.println(F("  - Or two devices are answering at once: unplug"));
        Serial.println(F("    every slave except the relay and rerun"));
    }
}


/* ---- optional: reprogram the baud once found -------------------------- */

static void fix_baud(uint32_t target) {
    if (!s_found) {
        Serial.println(F("Run a scan first -- nothing found yet."));
        return;
    }
    uint16_t idx = (target == 9600) ? 2 : (target == 115200) ? 5 : 0xFFFF;
    if (idx == 0xFFFF) { Serial.println(F("Only 9600 / 115200 supported.")); return; }

    open_bus(s_found_baud, s_found_framing);   /* must speak at the CURRENT rate */

    uint8_t req[6] = { s_found_addr, 0x06,
                       (WS_REG_BAUD >> 8) & 0xFF, WS_REG_BAUD & 0xFF,
                       (uint8_t)(idx >> 8), (uint8_t)(idx & 0xFF) };
    uint8_t resp[32];
    size_t n = txrx(req, sizeof(req), resp, sizeof(resp));

    Serial.print(F("FC06 -> 0x2000 = ")); Serial.print(idx);
    Serial.print(F("  reply: "));
    if (n) { print_hex(resp, n); } else { Serial.print(F("(none)")); }
    Serial.println();

    /* The board switches immediately, so verify at the NEW rate. */
    delay(200);
    open_bus(target, s_found_framing);
    if (probe_addr(s_found_addr)) {
        Serial.print(F("CONFIRMED at ")); Serial.println(target);
        s_found_baud = target;
    } else {
        Serial.println(F("No reply at the new rate -- rerun the scan."));
    }
}


/* ---- Arduino entry points --------------------------------------------- */

static void run_scan(void) {
    s_found = false; s_garbage_seen = 0; s_bytes_seen = 0;

    Serial.println(F("\nScanning... (8N1 first, then parity variants)"));
    for (uint8_t f = 0; f < N_FRAMINGS; f++) {
        if (sweep(FRAMINGS[f], FRAMING_NAMES[f])) break;
        Serial.print(F("\n-- nothing on ")); Serial.print(FRAMING_NAMES[f]);
        Serial.println(F(", trying next framing --"));
    }
    report();
    Serial.println(F("\nType SCAN to run again."));
}

void setup(void) {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println(F("=== Waveshare relay RS-485 scanner ==="));
    Serial.print(F("RX=")); Serial.print(PIN_RS485_RX);
    Serial.print(F(" TX=")); Serial.print(PIN_RS485_TX);
    Serial.print(F(" DE/RE=")); Serial.println(PIN_RS485_DE_RE);
    Serial.print(F("addresses ")); Serial.print(SCAN_ADDR_MIN);
    Serial.print(F("..")); Serial.println(SCAN_ADDR_MAX);

    pinMode(PIN_RS485_DE_RE, OUTPUT);
    digitalWrite(PIN_RS485_DE_RE, LOW);        /* listen by default */

    run_scan();
}

void loop(void) {
    static char line[16];
    static uint8_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0'; len = 0;
            if      (!strcasecmp(line, "SCAN"))      run_scan();
            else if (!strcasecmp(line, "FIX9600"))   fix_baud(9600);
            else if (!strcasecmp(line, "FIX115200")) fix_baud(115200);
            else if (line[0]) Serial.println(F("Commands: SCAN | FIX9600 | FIX115200"));
            continue;
        }
        if (len < sizeof(line) - 1) line[len++] = c;
    }
}
