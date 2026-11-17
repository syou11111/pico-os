#include "console.h"
#include "../drivers/st7735.h"
#include "../kernel/kernel.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define STATUS_BAR_H   14
#define CONSOLE_TOP    17
#define CONSOLE_COLS   26
#define CONSOLE_ROWS   11
#define LINE_HEIGHT    10
#define BG_COLOR       COLOR_BLACK

static char     lines[CONSOLE_ROWS][CONSOLE_COLS + 1];
static uint16_t line_colors[CONSOLE_ROWS];
static int      cur_row = 0;
static int      cur_col = 0;
static bool     statusbar_active = true;

static void redraw_row(int row) {
    uint16_t y = CONSOLE_TOP + row * LINE_HEIGHT;
    st7735_fill_rect(0, y, ST7735_WIDTH, LINE_HEIGHT, BG_COLOR);
    if (lines[row][0] != '\0') {
        st7735_draw_string(2, y + 1, lines[row], line_colors[row], BG_COLOR, 1);
    }
}

static void scroll_up(void) {
    for (int r = 0; r < CONSOLE_ROWS - 1; r++) {
        memcpy(lines[r], lines[r + 1], CONSOLE_COLS + 1);
        line_colors[r] = line_colors[r + 1];
    }
    memset(lines[CONSOLE_ROWS - 1], 0, CONSOLE_COLS + 1);
    line_colors[CONSOLE_ROWS - 1] = COLOR_WHITE;

    for (int r = 0; r < CONSOLE_ROWS; r++) {
        redraw_row(r);
    }
    cur_row = CONSOLE_ROWS - 1;
    cur_col = 0;
}

static void newline(uint16_t next_color) {
    cur_col = 0;
    if (cur_row < CONSOLE_ROWS - 1) {
        cur_row++;
        line_colors[cur_row] = next_color;
    } else {
        scroll_up();
        line_colors[cur_row] = next_color;
    }
}

void console_init(void) {
    statusbar_active = true;
    st7735_fill_screen(BG_COLOR);
    st7735_fill_rect(0, 0, ST7735_WIDTH, STATUS_BAR_H, COLOR_NAVY);
    st7735_draw_hline(0, STATUS_BAR_H, ST7735_WIDTH, COLOR_TEAL);
    st7735_draw_string(4, 3, "PicoOS v0.5", COLOR_CYAN, COLOR_NAVY, 1);
    console_clear();
}

void console_redraw_all(void) {
    st7735_fill_screen(BG_COLOR);
    st7735_fill_rect(0, 0, ST7735_WIDTH, STATUS_BAR_H, COLOR_NAVY);
    st7735_draw_hline(0, STATUS_BAR_H, ST7735_WIDTH, COLOR_TEAL);
    st7735_draw_string(4, 3, "PicoOS v0.5", COLOR_CYAN, COLOR_NAVY, 1);
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        redraw_row(r);
    }
    console_update_statusbar(os_get_ticks(), os_get_task_count(), true);
}

void console_set_statusbar_active(bool active) {
    statusbar_active = active;
}

void console_clear(void) {
    memset(lines, 0, sizeof(lines));
    for (int r = 0; r < CONSOLE_ROWS; r++) {
        line_colors[r] = COLOR_WHITE;
    }
    cur_row = 0;
    cur_col = 0;
    st7735_fill_rect(0, CONSOLE_TOP - 2, ST7735_WIDTH, ST7735_HEIGHT - (CONSOLE_TOP - 2), BG_COLOR);
}

void console_puts(const char *str, uint16_t color) {
    if (!str) return;
    line_colors[cur_row] = color;
    int start_row = cur_row;

    while (*str) {
        char c = *str++;
        if (c == '\r') continue;
        if (c == '\n') {
            redraw_row(cur_row);
            newline(color);
            start_row = cur_row;
            continue;
        }
        if (cur_col >= CONSOLE_COLS) {
            redraw_row(cur_row);
            newline(color);
            start_row = cur_row;
        }
        lines[cur_row][cur_col++] = c;
        lines[cur_row][cur_col] = '\0';
    }

    if (cur_row == start_row) {
        redraw_row(cur_row);
    }
}

void console_printf(uint16_t color, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_puts(buf, color);
}

void console_update_statusbar(uint32_t ticks, uint32_t task_count, bool heartbeat) {
    if (!statusbar_active) return;
    uint32_t secs = ticks / 1000;
    uint32_t mins = (secs / 60) % 100;
    uint32_t s = secs % 60;

    char info[24];
    snprintf(info, sizeof(info), "T:%lu %02lu:%02lu",
             (unsigned long)task_count,
             (unsigned long)mins,
             (unsigned long)s);

    st7735_draw_string(82, 3, info, COLOR_WHITE, COLOR_NAVY, 1);
    st7735_fill_rect(150, 4, 6, 6, heartbeat ? COLOR_GREEN : COLOR_DARKGRAY);
}
