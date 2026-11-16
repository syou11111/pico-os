#ifndef PICO_OS_CONSOLE_H
#define PICO_OS_CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

void console_init(void);
void console_clear(void);
void console_redraw_all(void);
void console_set_statusbar_active(bool active);
void console_puts(const char *str, uint16_t color);
void console_printf(uint16_t color, const char *fmt, ...);
void console_update_statusbar(uint32_t ticks, uint32_t task_count, bool heartbeat);

#endif /* PICO_OS_CONSOLE_H */
