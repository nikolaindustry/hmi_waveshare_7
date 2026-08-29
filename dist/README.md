# Hyperwisor S3 HMI — single-file firmware

`hyperwisor_s3_merged.bin` — one file containing bootloader, partition
table, OTA data and the application. Flash it at **offset 0x0**.

| | |
|---|---|
| Target chip | **ESP32-S3** |
| Flash offset | **0x0** |
| Flash mode / freq / size | DIO / 80 MHz / 16 MB |
| Size | ~2.8 MB |

Nothing else is needed — no separate bootloader or partition-table file.

---

## Flashing

### esptool (any OS, command line)

```bash
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 hyperwisor_s3_merged.bin
```

Add `-p <port>` if it doesn't auto-detect (`/dev/cu.usbmodemXXXX` on
macOS, `/dev/ttyACM0` on Linux, `COM4` on Windows).

### Espressif Flash Download Tool (Windows GUI)

1. Chip type: **ESP32-S3**, WorkMode: Develop
2. Add one row: `hyperwisor_s3_merged.bin` @ **`0x0`** — tick its checkbox
3. SPI: **DIO**, **80 MHz**, **16 MB**
4. Pick the COM port, **START**

### Browser (ESP Web Tools / esptool-js)

Choose the file, set offset **`0x0`**, chip **ESP32-S3**. Needs Chrome
or Edge (Web Serial).

---

## Connecting the board

Use the **native USB-C port** (shows up as `usbmodem` / "USB
JTAG/serial debug unit"), not the UART/CH343 port. With DIP switch #15
in the UART2 position — which it must be for RS-485 to work — the CH340
bridge is disconnected from the chip and cannot flash it.

If the board won't enter download mode: hold **BOOT**, tap **RST**,
release **BOOT**, then flash.

---

## Erase or not?

**Do not erase** unless you mean to. Settings live in the NVS
partition, which a plain `write_flash` at 0x0 leaves untouched:

- HMI role (PRIMARY / SECONDARY / WIRELESS)
- Wireless pairing (peer MAC + key)
- Owner name, tile names, theme, brightness, bus baud
- Saved RGB / star-roof colours

So an existing unit keeps its identity across an update — which is what
you want in the field.

Erasing (`esptool.py erase_flash`, or "Erase" in the GUI) resets a unit
to factory: role back to **PRIMARY**, unpaired, defaults everywhere. Use
it for a fresh board or a deliberate factory reset, then set the role and
re-pair.

---

## After flashing a new unit

1. Maintenance → **HMI Role** — pick PRIMARY, SECONDARY (wired) or
   WIRELESS, then let it reboot
2. For a wireless pair: tap **PAIR WIRELESS** on the primary (60 s
   window); the wireless unit finds it and locks on
3. Maintenance → **LED Strips** — set roof/floor LED counts if the
   vehicle has addressable strips

## Rebuilding this file

```bash
idf.py build
idf.py merge-bin -o "$PWD/dist/hyperwisor_s3_merged.bin"
```

Requires **ESP-IDF 5.5** (not 6.x — the HSC security module uses the
mbedTLS 3.x ECC API, which mbedTLS 4 removed).
