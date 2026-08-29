/*
 * board.h - Waveshare ESP32-S3-GEEK pin map.
 *
 * Every value below was taken from Waveshare's own ESP32-S3-GEEK demo sources
 * (Arduino DEV_Config.h / LCD_Driver / 07_SD_Test), not guessed:
 *   LCD  ST7789   DC=8 CS=10 RST=9 BL=7(PWM), SPI clk=12 mosi=11
 *   SD   (own bus) CLK=36 MOSI=35 MISO=37 CS=34
 *   BTN  BOOT = GPIO0 (active low)
 * The ST7789 on this board is a 135x240 panel viewed in 240x135 landscape,
 * with a controller RAM offset of 52 columns / 40 rows.
 */
#ifndef AEGIS_BOARD_H
#define AEGIS_BOARD_H

/* --- 1.14" ST7789 LCD (SPI2/HSPI) --- */
#define BOARD_LCD_SPI_HOST     SPI2_HOST
#define BOARD_LCD_PIN_SCLK     12
#define BOARD_LCD_PIN_MOSI     11
#define BOARD_LCD_PIN_CS       10
#define BOARD_LCD_PIN_DC        8
#define BOARD_LCD_PIN_RST       9
#define BOARD_LCD_PIN_BL        7
#define BOARD_LCD_H_RES       240   /* landscape width  */
#define BOARD_LCD_V_RES       135   /* landscape height */
#define BOARD_LCD_X_GAP        52   /* native column offset */
#define BOARD_LCD_Y_GAP        40   /* native row offset    */

/* --- microSD / TF card (its own SPI bus) --- */
#define BOARD_SD_SPI_HOST      SPI3_HOST
#define BOARD_SD_PIN_SCLK      36
#define BOARD_SD_PIN_MOSI      35
#define BOARD_SD_PIN_MISO      37
#define BOARD_SD_PIN_CS        34

/* --- BOOT button (also used as the single UI control) --- */
#define BOARD_BTN_PIN           0    /* active low */

#endif /* AEGIS_BOARD_H */
