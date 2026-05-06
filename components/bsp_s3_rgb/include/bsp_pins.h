/* Waveshare ESP32-S3-Touch-LCD-7 pin map
 * Values verified against Waveshare official 08_lvgl_Porting demo.
 */
#pragma once

/* ---- I2C (shared: CH422G IO expander + GT911 touch) ---- */
#define BSP_I2C_NUM        0
#define BSP_I2C_SDA        8
#define BSP_I2C_SCL        9
#define BSP_I2C_FREQ_HZ    400000
#define BSP_I2C_TIMEOUT_MS 1000

/* ---- CH422G I/O expander ---- */
/* CH422G uses multiple fixed I2C slave addresses, one per register */
#define CH422G_ADDR_WR_SET   0x24    /* system parameter (IO_OE, ...) */
#define CH422G_ADDR_WR_IO    0x38    /* IO0..IO7 output latch */

/* CH422G output bit assignment on this specific board
 * (decoded from Waveshare's wavesahre_rgb_lcd_bl_on/off which writes 0x1E/0x1A)
 */
#define CH422G_IO_TP_RST     1      /* GT911 RESET (release = high) */
#define CH422G_IO_LCD_BL     2      /* Backlight enable */
#define CH422G_IO_DISP       3      /* LCD DISP / power enable (keep high) */
#define CH422G_IO_SD_CS      4      /* SD card CS (high = deassert) */
#define CH422G_IO_USB_SEL    5      /* USB mode select (unused) */

/* Canonical initial IO byte: DISP+SD_CS high, rest low (touch in reset, BL off) */
#define CH422G_IO_INIT_VAL   ((1u << CH422G_IO_DISP) | (1u << CH422G_IO_SD_CS))
/* Backlight ON state: TP released + BL on + DISP + SD_CS = 0x1E */
#define CH422G_IO_BL_ON_VAL  0x1E
/* Backlight OFF state: TP released + BL off + DISP + SD_CS = 0x1A */
#define CH422G_IO_BL_OFF_VAL 0x1A

/* ---- RGB LCD (800x480) ---- */
#define BSP_LCD_H_RES              800
#define BSP_LCD_V_RES              480
#define BSP_LCD_BIT_PER_PIXEL      16
#define BSP_LCD_DATA_WIDTH         16
#define BSP_LCD_PCLK_HZ            (16 * 1000 * 1000)

#define BSP_LCD_IO_HSYNC           46
#define BSP_LCD_IO_VSYNC           3
#define BSP_LCD_IO_DE              5
#define BSP_LCD_IO_PCLK            7
#define BSP_LCD_IO_DISP            (-1)   /* DISP is driven via CH422G, not a GPIO */

/* DATA0..DATA15 order per Waveshare reference (CRITICAL — do not reshuffle) */
#define BSP_LCD_IO_DATA0           14    /* B3 */
#define BSP_LCD_IO_DATA1           38    /* B4 */
#define BSP_LCD_IO_DATA2           18    /* B5 */
#define BSP_LCD_IO_DATA3           17    /* B6 */
#define BSP_LCD_IO_DATA4           10    /* B7 */
#define BSP_LCD_IO_DATA5           39    /* G2 */
#define BSP_LCD_IO_DATA6           0     /* G3 */
#define BSP_LCD_IO_DATA7           45    /* G4 */
#define BSP_LCD_IO_DATA8           48    /* G5 */
#define BSP_LCD_IO_DATA9           47    /* G6 */
#define BSP_LCD_IO_DATA10          21    /* G7 */
#define BSP_LCD_IO_DATA11          1     /* R3 */
#define BSP_LCD_IO_DATA12          2     /* R4 */
#define BSP_LCD_IO_DATA13          42    /* R5 */
#define BSP_LCD_IO_DATA14          41    /* R6 */
#define BSP_LCD_IO_DATA15          40    /* R7 */

/* RGB panel timings (7" 800x480) */
#define BSP_LCD_HSYNC_PULSE_WIDTH  4
#define BSP_LCD_HSYNC_BACK_PORCH   8
#define BSP_LCD_HSYNC_FRONT_PORCH  8
#define BSP_LCD_VSYNC_PULSE_WIDTH  4
#define BSP_LCD_VSYNC_BACK_PORCH   8
#define BSP_LCD_VSYNC_FRONT_PORCH  8

/* Bounce buffer height (lines) — H_RES*height words transferred per burst */
#define BSP_LCD_BOUNCE_BUFFER_HEIGHT  10

/* ---- GT911 touch ---- */
/* INT pin set to -1: GPIO4 is only used transiently as OUTPUT during reset
 * dance (to force I2C address = 0x5D). No interrupt mode. */
#define BSP_TOUCH_INT              (-1)
#define BSP_TOUCH_RST              (-1)   /* RESET is via CH422G, not a GPIO */
#define BSP_TOUCH_RESET_IO4        4      /* GPIO used during reset-time addr selection */

/* ---- Onboard RS-485 (auto direction via MAX13487-class transceiver) ----
 * NOTE: requires board switch #15 to be in the UART2 position, otherwise
 * GPIO15/16 are captured by the CH340 USB-UART bridge. With switch in
 * UART2 position, the CH340 Type-C port is offline and flashing must be
 * done via the native USB-JTAG-SERIAL Type-C port (GPIO19/20 internal).
 * Pin map from Waveshare ESP32-S3-Touch-LCD-7 wiki, RS485 Interface. */
#define BSP_RS485_UART_NUM         1       /* UART1 peripheral */
/* WAVESHARE QUIRK: silkscreen labels GPIO15=TX/16=RX are misleading on this
 * board. The onboard auto-direction circuit only triggers when the ESP32-S3
 * UART TX is bound to GPIO16 (NOT 15). Verified empirically with the bare
 * modbus_test sketch: with TX=15/RX=16 we got 0 bytes; with TX=16/RX=15 the
 * relay board replied on the first frame. Do NOT swap these back. */
#define BSP_RS485_IO_TX            16      /* RS485 TX (drives auto-DE circuit) */
#define BSP_RS485_IO_RX            15      /* RS485 RX */
/* Factory-safe default baud. Matches unconfigured Waveshare relay board.
 * Effective runtime baud is NVS-backed via bus_config; user upgrades to
 * 115200 from Maintenance → Bus Setup on the HMI itself (no external tools). */
#define BSP_RS485_DEFAULT_BAUD     9600
