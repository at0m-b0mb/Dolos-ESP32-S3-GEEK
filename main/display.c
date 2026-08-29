#include "display.h"
#include "board.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;               /* 240*135 RGB565 framebuffer   */
static canvas_t  s_canvas;
static bool      s_ok;

#define FB_PIX (BOARD_LCD_H_RES * BOARD_LCD_V_RES)

bool display_init(void)
{
    s_fb = heap_caps_malloc(FB_PIX * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) s_fb = heap_caps_malloc(FB_PIX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb) { ESP_LOGE(TAG, "no memory for framebuffer"); return false; }
    cv_init(&s_canvas, s_fb, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    gpio_config_t bl = { .pin_bit_mask = 1ULL << BOARD_LCD_PIN_BL,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_PIN_BL, 1);           /* backlight on */

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = FB_PIX * sizeof(uint16_t) + 16,
    };
    if (spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi(BOARD_LCD_SPI_HOST, &io_cfg, &io) != ESP_OK) return false;

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &pcfg, &s_panel) != ESP_OK) return false;

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);     /* GEEK panel uses INVON (0x21) */
    /* 135x240 native -> 240x135 landscape. swap_xy + mirror(false,true) is the
     * 270-degree landscape (USB plug to the right, text upright), verified on
     * hardware. mirror(true,false) is the 180-degree-flipped sibling. */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, false, true);
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_Y_GAP, BOARD_LCD_X_GAP);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_ok = true;
    ESP_LOGI(TAG, "ST7789 up (%dx%d landscape)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return true;
}

canvas_t *display_canvas(void) { return &s_canvas; }

void display_flush(void)
{
    if (!s_ok) return;
    /* ST7789 wants big-endian 16-bit on the wire; our canvas is little-endian.
     * We re-render the whole frame each tick, so an in-place byte swap here is
     * safe. */
    for (int i = 0; i < FB_PIX; i++) s_fb[i] = (uint16_t)((s_fb[i] << 8) | (s_fb[i] >> 8));
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fb);
}
