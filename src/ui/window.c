#include "window.h"
#include "console.h"
#include "../drivers/st7735.h"
#include <stdio.h>
#include <string.h>

static bool win_active = false;
static char win_title[24] = "App";

static void draw_chrome(void) {
    uint16_t bar_bg = RGB565(20, 44, 84);
    st7735_fill_rect(0, 0, ST7735_WIDTH, WIN_TITLE_H, bar_bg);
    st7735_draw_string(4, 3, win_title, COLOR_CYAN, bar_bg, 1);

    /* Close badge [x] */
    st7735_fill_rect(146, 2, 11, 10, RGB565(150, 32, 32));
    st7735_draw_string(149, 3, "x", COLOR_WHITE, RGB565(150, 32, 32), 1);

    /* Frame border around client area */
    st7735_draw_hline(0, WIN_TITLE_H, ST7735_WIDTH, COLOR_TEAL);
    st7735_draw_hline(0, ST7735_HEIGHT - 1, ST7735_WIDTH, COLOR_TEAL);
    st7735_fill_rect(0, WIN_TITLE_H, 1, ST7735_HEIGHT - WIN_TITLE_H, COLOR_TEAL);
    st7735_fill_rect(ST7735_WIDTH - 1, WIN_TITLE_H, 1, ST7735_HEIGHT - WIN_TITLE_H, COLOR_TEAL);
}

void win_open(const char *title) {
    if (title && title[0] != '\0') {
        snprintf(win_title, sizeof(win_title), "%.21s", title);
    } else {
        snprintf(win_title, sizeof(win_title), "GUI Window");
    }
    win_active = true;
    console_set_gui_mode(true);
    console_set_statusbar_active(false);
    draw_chrome();
    st7735_fill_rect(WIN_CLIENT_X, WIN_CLIENT_Y, WIN_CLIENT_W, WIN_CLIENT_H, COLOR_BLACK);
}

void win_set_title(const char *title) {
    if (!win_active) return;
    if (title && title[0] != '\0') {
        snprintf(win_title, sizeof(win_title), "%.21s", title);
    }
    draw_chrome();
}

void win_close(void) {
    if (!win_active) return;
    win_active = false;
    console_set_gui_mode(false);
    console_set_statusbar_active(true);
    console_redraw_all();
}

bool win_is_open(void) {
    return win_active;
}

static int32_t map_x(int32_t vx) {
    return WIN_CLIENT_X + (vx * WIN_CLIENT_W) / ST7735_WIDTH;
}

static int32_t map_y(int32_t vy) {
    return WIN_CLIENT_Y + (vy * WIN_CLIENT_H) / ST7735_HEIGHT;
}

void win_cls(uint16_t color) {
    if (!win_active) win_open("GUI");
    st7735_fill_rect(WIN_CLIENT_X, WIN_CLIENT_Y, WIN_CLIENT_W, WIN_CLIENT_H, color);
}

void win_rect(int32_t vx, int32_t vy, int32_t vw, int32_t vh, uint16_t color) {
    if (!win_active) win_open("GUI");
    if (vw <= 0 || vh <= 0) return;

    int32_t x1 = map_x(vx);
    int32_t y1 = map_y(vy);
    int32_t w = (vw * WIN_CLIENT_W + ST7735_WIDTH - 1) / ST7735_WIDTH;
    int32_t h = (vh * WIN_CLIENT_H + ST7735_HEIGHT - 1) / ST7735_HEIGHT;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int32_t x2 = x1 + w;
    int32_t y2 = y1 + h;

    if (x1 < WIN_CLIENT_X) x1 = WIN_CLIENT_X;
    if (y1 < WIN_CLIENT_Y) y1 = WIN_CLIENT_Y;
    if (x2 > WIN_CLIENT_X + WIN_CLIENT_W) x2 = WIN_CLIENT_X + WIN_CLIENT_W;
    if (y2 > WIN_CLIENT_Y + WIN_CLIENT_H) y2 = WIN_CLIENT_Y + WIN_CLIENT_H;

    if (x2 > x1 && y2 > y1) {
        st7735_fill_rect((uint16_t)x1, (uint16_t)y1, (uint16_t)(x2 - x1), (uint16_t)(y2 - y1), color);
    }
}

void win_text(int32_t vx, int32_t vy, const char *s, uint16_t fg, uint16_t bg) {
    if (!win_active) win_open("GUI");
    if (!s) return;

    int32_t x = map_x(vx);
    int32_t y = map_y(vy);
    if (x < WIN_CLIENT_X) x = WIN_CLIENT_X;
    if (y < WIN_CLIENT_Y) y = WIN_CLIENT_Y;
    if (x >= WIN_CLIENT_X + WIN_CLIENT_W - 5) return;
    if (y >= WIN_CLIENT_Y + WIN_CLIENT_H - 7) return;

    st7735_draw_string((uint16_t)x, (uint16_t)y, s, fg, bg, 1);
}

void win_pixel(int32_t vx, int32_t vy, uint16_t color) {
    if (!win_active) win_open("GUI");
    int32_t x = map_x(vx);
    int32_t y = map_y(vy);
    if (x < WIN_CLIENT_X || x >= WIN_CLIENT_X + WIN_CLIENT_W) return;
    if (y < WIN_CLIENT_Y || y >= WIN_CLIENT_Y + WIN_CLIENT_H) return;
    st7735_draw_pixel((uint16_t)x, (uint16_t)y, color);
}
