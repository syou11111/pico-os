#ifndef PICO_OS_ST7735_H
#define PICO_OS_ST7735_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/types.h"

/* Default pin assignments (SPI0) */
#define ST7735_DEFAULT_CS    17
#define ST7735_DEFAULT_SCK   18
#define ST7735_DEFAULT_MOSI  19
#define ST7735_DEFAULT_RST   20
#define ST7735_DEFAULT_DC    21
#define ST7735_DEFAULT_BLK   22

/* Display resolution in landscape mode (1.8" 128x160 ST7735S) */
#define ST7735_WIDTH     160
#define ST7735_HEIGHT    128
#define ST7735_XSTART    0
#define ST7735_YSTART    0

/* RGB565 Colors */
#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK      0x0000
#define COLOR_WHITE      0xFFFF
#define COLOR_RED        0xF800
#define COLOR_GREEN      0x07E0
#define COLOR_BLUE       0x001F
#define COLOR_CYAN       0x07FF
#define COLOR_MAGENTA    0xF81F
#define COLOR_YELLOW     0xFFE0
#define COLOR_ORANGE     0xFD20
#define COLOR_DARKGRAY   RGB565(32, 36, 44)
#define COLOR_NAVY       RGB565(12, 24, 48)
#define COLOR_TEAL       RGB565(0, 140, 160)

typedef struct __attribute__((packed)) {
    uint8_t spi_bus;
    uint8_t cs;
    uint8_t sck;
    uint8_t mosi;
    uint8_t rst;
    uint8_t dc;
    uint8_t blk;
} st7735_pins_t;

void st7735_init(void);
void st7735_set_backlight(uint8_t percent);
void st7735_fill_screen(uint16_t color);
void st7735_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7735_draw_pixel(uint16_t x, uint16_t y, uint16_t color);
void st7735_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void st7735_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void st7735_draw_string(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

const st7735_pins_t *st7735_get_pins(void);
bool st7735_is_display_pin(uint pin);
void st7735_load_saved_pins(void);
int  st7735_reconfigure_pins(uint8_t cs, uint8_t sck, uint8_t mosi,
                             uint8_t rst, uint8_t dc, uint8_t blk, bool persist);
int  st7735_reset_pins(void);

#endif /* PICO_OS_ST7735_H */
