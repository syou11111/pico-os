#ifndef PICO_OS_WINDOW_H
#define PICO_OS_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#define WIN_TITLE_H   14
#define WIN_CLIENT_X  1
#define WIN_CLIENT_Y  15
#define WIN_CLIENT_W  158
#define WIN_CLIENT_H  112

void win_open(const char *title);
void win_set_title(const char *title);
void win_close(void);
bool win_is_open(void);

void win_cls(uint16_t color);
void win_rect(int32_t vx, int32_t vy, int32_t vw, int32_t vh, uint16_t color);
void win_text(int32_t vx, int32_t vy, const char *s, uint16_t fg, uint16_t bg);
void win_pixel(int32_t vx, int32_t vy, uint16_t color);

#endif /* PICO_OS_WINDOW_H */
