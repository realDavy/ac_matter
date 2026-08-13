/*
   ESP32-S3-WROOM-1-N16 default pin map for AC Remote + 1.28" GC9A01/IT7259.

   Avoids module flash (GPIO26-32), USB (GPIO19/20), and keeps BOOT on GPIO0.
   PCB authors may remultiplex free GPIOs, but keep these defaults in firmware.
*/

#pragma once

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <driver/spi_master.h>

/* ---- IR (RMT) ---- */
#define BOARD_IR_TX_GPIO          GPIO_NUM_5
#define BOARD_IR_RX_GPIO          GPIO_NUM_4

/* ---- Shared I2C: SHT30 + IT7259 ---- */
#define BOARD_I2C_PORT            I2C_NUM_0
#define BOARD_I2C_SDA_GPIO        GPIO_NUM_8
#define BOARD_I2C_SCL_GPIO        GPIO_NUM_9
#define BOARD_I2C_FREQ_HZ         100000

#define BOARD_IT7259_ADDR         0x46
#define BOARD_IT7259_INT_GPIO     GPIO_NUM_15
#define BOARD_IT7259_RST_GPIO     GPIO_NUM_16

/* ---- WS2812 ambient light ---- */
#define BOARD_WS2812_GPIO         GPIO_NUM_10

/* ---- Status LED (active-low) ---- */
#define BOARD_STATUS_LED_GPIO     GPIO_NUM_11
#define BOARD_STATUS_LED_ACTIVE_LOW 1

/* ---- BOOT / factory / Alt (GPIO0) ---- */
#define BOARD_BOOT_BUTTON_GPIO    GPIO_NUM_0

/* ---- 1.28" GC9A01 circular LCD (4-line SPI) ---- */
#define BOARD_LCD_HOST            SPI2_HOST
#define BOARD_LCD_SCLK_GPIO       GPIO_NUM_12
#define BOARD_LCD_MOSI_GPIO       GPIO_NUM_13
#define BOARD_LCD_CS_GPIO         GPIO_NUM_14
#define BOARD_LCD_DC_GPIO         GPIO_NUM_21
#define BOARD_LCD_RST_GPIO        GPIO_NUM_47
#define BOARD_LCD_BL_GPIO         GPIO_NUM_48
#define BOARD_LCD_H_RES           240
#define BOARD_LCD_V_RES           240
#define BOARD_LCD_SPI_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
/*
 * This GC9A01 module shows LVGL content horizontally reversed unless MX is
 * set (EN→NE, 汉字左右颠倒). Correct scan direction for this panel.
 *
 * Touch (IT7259) reports glass/physical coordinates. After MADCTL MX, LVGL
 * logical X already matches the glass — do NOT also invert touch or left/right
 * taps hit the opposite side. Override BOARD_TOUCH_MIRROR_* only if a unit
 * still mismatches after flash.
 */
#define BOARD_LCD_MIRROR_X        1
#define BOARD_LCD_MIRROR_Y        0
#define BOARD_TOUCH_MIRROR_X      0
#define BOARD_TOUCH_MIRROR_Y      0
