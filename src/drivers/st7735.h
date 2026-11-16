#ifndef PICO_OS_ST7735_H
#define PICO_OS_ST7735_H

#include <stdint.h>
#include <stdbool.h>

/* Pin assignments (SPI0) */
#define ST7735_SPI_PORT  spi0
#define ST7735_PIN_CS    17
#define ST7735_PIN_SCK   18
#define ST7735_PIN_MOSI  19
#define ST7735_PIN_RST   20
#define ST7735_PIN_DC    21
#define ST7735_PIN_BLK   22

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

void st7735_init(void);
void st7735_set_backlight(uint8_t percent);
void st7735_fill_screen(uint16_t color);
void st7735_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7735_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);
void st7735_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
void st7735_draw_string(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);

#endif /* PICO_OS_ST7735_H */
