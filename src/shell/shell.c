#include "shell.h"
#include "../kernel/kernel.h"
#include "../drivers/st7735.h"
#include "../drivers/hw_io.h"
#include "../ui/console.h"
#include "../fs/fs.h"
#include "../apps/vi.h"
#include "../apps/cc.h"
#include "../transfer/transfer.h"
#include "../net/wifi_ap.h"
#include "../net/ble_uart.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern volatile bool g_led_auto_blink;
extern void set_onboard_led(bool on);

void shell_seed_samples(void) {
    static const char hello_c[] =
        "#include <stdio.h>\n"
        "\n"
        "int main(void) {\n"
        "    printf(\"Hello from PicoOS C!\\n\");\n"
        "    printf(\"Uptime ticks: %d\\n\", ticks());\n"
        "    return 0;\n"
        "}\n";

    static const char fib_c[] =
        "#include <stdio.h>\n"
        "\n"
        "int fib(int n) {\n"
        "    if (n <= 1) return n;\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    for (int i = 0; i <= 12; i++) {\n"
        "        printf(\"fib(%2d) = %d\\n\", i, fib(i));\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    static const char gfx_c[] =
        "#define W 160\n"
        "#define H 128\n"
        "\n"
        "int main(void) {\n"
        "    cls(0);\n"
        "    for (int i = 0; i < 8; i++) {\n"
        "        int c = rgb(i * 32, 255 - i * 28, 180);\n"
        "        rect(10 + i * 18, 28, 14, 70, c);\n"
        "    }\n"
        "    text(14, 10, \"PicoOS C Graphics!\", rgb(0, 255, 255), 0);\n"
        "    text(14, 110, \"Run 'clear' to exit\", rgb(255, 255, 0), 0);\n"
        "    return 0;\n"
        "}\n";

    static const char sensor_c[] =
        "#include <stdio.h>\n"
        "#include <gpio.h>\n"
        "\n"
        "int main(void) {\n"
        "    printf(\"=== PicoOS Sensor Demo ===\\n\");\n"
        "    for (int i = 0; i < 5; i++) {\n"
        "        printf(\"[%d] Temp: %d C | GP26: %d\\n\",\n"
        "               i + 1, temp_c(), adc_read(26));\n"
        "        sleep_ms(300);\n"
        "    }\n"
        "    return 0;\n"
        "}\n";

    static const char game_c[] =
        "#include <stdio.h>\n"
        "#include <gpio.h>\n"
        "\n"
        "int main(void) {\n"
        "    int bg = rgb(8, 12, 24);\n"
        "    int px = 64, old_px = 64;\n"
        "    int bx = 80, by = 40, ox = 80, oy = 40;\n"
        "    int vx = 2, vy = 2, score = 0;\n"
        "    cls(bg);\n"
        "    text(4, 2, \"PADDLE GAME (A/D, Q=quit)\", rgb(0, 255, 255), bg);\n"
        "    rect(0, 12, 160, 1, rgb(0, 160, 180));\n"
        "    printf(\"Paddle Game: A/D or Left/Right, Q to quit\\n\");\n"
        "    while (1) {\n"
        "        int k = key_poll();\n"
        "        if (k == 'q' || k == 'Q' || k == KEY_ESC) break;\n"
        "        if (k == 'a' || k == 'A' || k == KEY_LEFT) px -= 10;\n"
        "        if (k == 'd' || k == 'D' || k == KEY_RIGHT) px += 10;\n"
        "        if (px < 2) px = 2;\n"
        "        if (px > 126) px = 126;\n"
        "        bx += vx; by += vy;\n"
        "        if (bx <= 2 || bx >= 154) vx = -vx;\n"
        "        if (by <= 15) vy = -vy;\n"
        "        if (by >= 112 && by <= 117 && bx + 4 >= px && bx <= px + 32 && vy > 0) {\n"
        "            vy = -vy;\n"
        "            score++;\n"
        "            printf(\"Score: %d\\n\", score);\n"
        "        }\n"
        "        if (by > 124) {\n"
        "            text(48, 60, \"GAME OVER!\", rgb(255, 60, 60), bg);\n"
        "            printf(\"Game Over! Final Score: %d\\n\", score);\n"
        "            break;\n"
        "        }\n"
        "        if (old_px != px) rect(old_px, 118, 32, 5, bg);\n"
        "        rect(ox, oy, 4, 4, bg);\n"
        "        rect(px, 118, 32, 5, rgb(0, 255, 128));\n"
        "        rect(bx, by, 4, 4, rgb(255, 220, 0));\n"
        "        old_px = px; ox = bx; oy = by;\n"
        "        sleep_ms(30);\n"
        "    }\n"
        "    return score;\n"
        "}\n";

    fs_write("hello.c", hello_c, strlen(hello_c));
    fs_write("fib.c", fib_c, strlen(fib_c));
    fs_write("gfx.c", gfx_c, strlen(gfx_c));
    fs_write("sensor.c", sensor_c, strlen(sensor_c));
    fs_write("game.c", game_c, strlen(game_c));
}

static const char *state_name(task_state_t s) {
    switch (s) {
        case TASK_RUNNING:  return "RUN";
        case TASK_READY:    return "RDY";
        case TASK_SLEEPING: return "SLP";
        default:            return "---";
    }
}

static char *skip_spaces(char *s) {
    while (*s == ' ') s++;
    return s;
}

static char *split_arg(char *s, char **rest) {
    s = skip_spaces(s);
    if (*s == '\0') {
        *rest = s;
        return NULL;
    }
    char *start = s;
    while (*s && *s != ' ') s++;
    if (*s == ' ') {
        *s++ = '\0';
    }
    *rest = skip_spaces(s);
    return start;
}

static void handle_cc_command(char *args) {
    char *rest = NULL;
    char *a1 = split_arg(args, &rest);
    if (!a1) {
        printf("Usage: cc <file.c> [-o <out>] | cc -r <file.c>\r\n");
        return;
    }
    if (strcmp(a1, "-r") == 0) {
        char *src = split_arg(rest, &rest);
        if (!src) {
            printf("Usage: cc -r <file.c>\r\n");
            return;
        }
        cc_compile_and_run(src);
        return;
    }

    char *out = "a.out";
    char *a2 = split_arg(rest, &rest);
    if (a2 && strcmp(a2, "-o") == 0) {
        char *a3 = split_arg(rest, &rest);
        if (a3) out = a3;
    }
    cc_compile_file(a1, out);
}

static void handle_disp_command(char *args) {
    char *rest = NULL;
    char *sub = split_arg(args, &rest);
    if (!sub) {
        const st7735_pins_t *p = st7735_get_pins();
        printf("Display ST7735S (SPI%u):\r\n", (unsigned)p->spi_bus);
        printf("  CS=%u SCK=%u MOSI=%u RST=%u DC=%u BLK=%u\r\n",
               p->cs, p->sck, p->mosi, p->rst, p->dc, p->blk);
        printf("Usage:\r\n");
        printf("  disp set <cs> <sck> <mosi> <rst> <dc> <blk>\r\n");
        printf("  disp reset\r\n");
        console_printf(COLOR_WHITE, "SPI%u CS=%u SCK=%u TX=%u\n",
                       p->spi_bus, p->cs, p->sck, p->mosi);
        return;
    }
    if (strcmp(sub, "reset") == 0) {
        int rc = st7735_reset_pins();
        if (rc == 0) {
            console_set_statusbar_active(true);
            console_redraw_all();
            printf("Display pins reset to default (17..22) and saved.\r\n");
        } else {
            printf("disp reset failed (%d)\r\n", rc);
        }
        return;
    }
    if (strcmp(sub, "set") == 0) {
        int nums[6];
        for (int i = 0; i < 6; i++) {
            char *tok = split_arg(rest, &rest);
            if (!tok) {
                printf("Usage: disp set <cs> <sck> <mosi> <rst> <dc> <blk>\r\n");
                return;
            }
            nums[i] = atoi(tok);
        }
        int rc = st7735_reconfigure_pins(
            (uint8_t)nums[0], (uint8_t)nums[1], (uint8_t)nums[2],
            (uint8_t)nums[3], (uint8_t)nums[4], (uint8_t)nums[5], true);
        if (rc == 0) {
            console_set_statusbar_active(true);
            console_redraw_all();
            const st7735_pins_t *p = st7735_get_pins();
            printf("Display updated (SPI%u: CS=%u SCK=%u MOSI=%u RST=%u DC=%u BLK=%u) & saved.\r\n",
                   p->spi_bus, p->cs, p->sck, p->mosi, p->rst, p->dc, p->blk);
        } else {
            printf("disp set failed (%d): check valid SPI0/SPI1 pins & no duplicates.\r\n", rc);
        }
        return;
    }
    printf("Unknown disp subcommand: %s\r\n", sub);
}

static void handle_pinout_command(void) {
    const st7735_pins_t *p = st7735_get_pins();
    printf("Display (SPI%u): CS=GP%u SCK=GP%u MOSI=GP%u RST=GP%u DC=GP%u BLK=GP%u\r\n",
           p->spi_bus, p->cs, p->sck, p->mosi, p->rst, p->dc, p->blk);
    printf("Free User GPIOs: ");
    int col = 0;
    for (int gp = 0; gp <= 28; gp++) {
        if (hw_is_pin_safe(gp)) {
            printf("GP%-2d ", gp);
            if (++col % 8 == 0) printf("\r\n                 ");
        }
    }
    printf("\r\nADC Pins: GP26(ADC0), GP27(ADC1), GP28(ADC2), Temp(ADC4)\r\n");
    console_printf(COLOR_GREEN, "Disp SPI%u | %d free GPIOs\n", p->spi_bus, col);
}

static void handle_gpio_command(char *args) {
    char *rest = NULL;
    char *sub = split_arg(args, &rest);
    if (!sub) {
        printf("Usage: gpio mode <pin> <in|out|up|down> | gpio write <pin> <0|1> | gpio read <pin> | gpio adc <pin|temp>\r\n");
        return;
    }
    if (strcmp(sub, "mode") == 0) {
        char *ps = split_arg(rest, &rest);
        char *ms = split_arg(rest, &rest);
        if (!ps || !ms) { printf("Usage: gpio mode <pin> <in|out|up|down>\r\n"); return; }
        int pin = atoi(ps);
        int m = HW_INPUT;
        if (strcmp(ms, "out") == 0) m = HW_OUTPUT;
        else if (strcmp(ms, "up") == 0) m = HW_INPUT_PULLUP;
        else if (strcmp(ms, "down") == 0) m = HW_INPUT_PULLDOWN;
        int rc = hw_pin_mode(pin, m);
        printf(rc == 0 ? "GP%d mode=%s\r\n" : "Invalid/protected pin GP%d\r\n", pin, ms);
    } else if (strcmp(sub, "write") == 0) {
        char *ps = split_arg(rest, &rest);
        char *vs = split_arg(rest, &rest);
        if (!ps || !vs) { printf("Usage: gpio write <pin> <0|1>\r\n"); return; }
        int pin = atoi(ps), v = atoi(vs);
        int rc = hw_digital_write(pin, v);
        printf(rc == 0 ? "GP%d <- %d\r\n" : "Invalid/protected pin GP%d\r\n", pin, v ? 1 : 0);
    } else if (strcmp(sub, "read") == 0) {
        char *ps = split_arg(rest, &rest);
        if (!ps) { printf("Usage: gpio read <pin>\r\n"); return; }
        int pin = atoi(ps);
        int v = hw_digital_read(pin);
        if (v < 0) printf("Invalid/protected pin GP%d\r\n", pin);
        else printf("GP%d = %d\r\n", pin, v);
    } else if (strcmp(sub, "adc") == 0) {
        char *ps = split_arg(rest, &rest);
        if (!ps) { printf("Usage: gpio adc <pin|temp>\r\n"); return; }
        if (strcmp(ps, "temp") == 0) {
            printf("Internal Temp: %d C (raw=%d)\r\n", hw_read_temp_c(), hw_adc_read(4));
        } else {
            int pin = atoi(ps);
            int v = hw_adc_read(pin);
            if (v < 0) printf("Invalid/protected ADC pin %d\r\n", pin);
            else printf("ADC(%d) = %d\r\n", pin, v);
        }
    } else {
        printf("Unknown gpio subcommand: %s\r\n", sub);
    }
}

static void handle_i2c_command(char *args) {
    char *rest = NULL;
    char *sub = split_arg(args, &rest);
    if (!sub || strcmp(sub, "scan") != 0) {
        printf("Usage: i2c scan [bus] [sda] [scl]\r\n");
        return;
    }
    int bus = 0, sda = 4, scl = 5;
    char *b_s = split_arg(rest, &rest);
    char *d_s = split_arg(rest, &rest);
    char *c_s = split_arg(rest, &rest);
    if (b_s) bus = atoi(b_s);
    if (d_s) sda = atoi(d_s);
    if (c_s) scl = atoi(c_s);
    else if (bus == 1 && !d_s) { sda = 6; scl = 7; }

    int rc = hw_i2c_init(bus, sda, scl, 100000);
    if (rc < 0) {
        printf("i2c init failed (%d): check I2C%d pins (SDA=GP%d SCL=GP%d)\r\n", rc, bus, sda, scl);
        return;
    }
    uint8_t found[16];
    int n = hw_i2c_scan(bus, found, 16);
    printf("I2C%d (SDA=GP%d, SCL=GP%d): found %d device(s)\r\n", bus, sda, scl, n);
    for (int i = 0; i < n && i < 16; i++) {
        printf("  0x%02X\r\n", found[i]);
    }
    console_printf(COLOR_WHITE, "I2C%d scan: %d dev\n", bus, n);
}

static void exec_command(char *line) {
    line = skip_spaces(line);
    if (*line == '\0') return;

    console_printf(COLOR_CYAN, "> %s\n", line);

    if (strcmp(line, "help") == 0) {
        printf("Commands:\r\n");
        printf("  help                 Show this help\r\n");
        printf("  ps                   List running OS tasks\r\n");
        printf("  uptime               Show OS uptime\r\n");
        printf("  clear                Clear TFT console\r\n");
        printf("  echo <text>          Print to TFT & serial\r\n");
        printf("  bl <0-100>           Set TFT backlight %%\r\n");
        printf("  led <on|off|auto>    Control onboard LED\r\n");
        printf("  ls                   List files in PicoFS\r\n");
        printf("  cat <file>           Print file contents\r\n");
        printf("  write <file> <txt>   Create/overwrite file\r\n");
        printf("  append <file> <txt>  Append line to file\r\n");
        printf("  rm <file>            Delete file\r\n");
        printf("  df                   Show filesystem usage\r\n");
        printf("  format               Erase all files\r\n");
        printf("  vi <file>            Open vi modal text editor\r\n");
        printf("  cc <file.c> [-o out] Compile C source to bytecode\r\n");
        printf("  cc -r <file.c>       Compile and run immediately\r\n");
        printf("  run [bin] | ./<bin>  Execute compiled PXE1 binary\r\n");
        printf("  disasm [bin]         Disassemble PXE1 binary\r\n");
        printf("  rx <file>            Receive file from PC (framed/paste)\r\n");
        printf("  tx <file>            Send file to PC (Hex + CRC16)\r\n");
        printf("  pinout               Show display & free user GPIO pins\r\n");
        printf("  disp [set ...|reset] View/remap ST7735S display pins\r\n");
        printf("  gpio <mode|write|read|adc>  Test GPIO/ADC pins\r\n");
        printf("  i2c scan [bus sda scl]      Scan I2C bus devices\r\n");
        printf("  ble                  Show BLE UART connection status\r\n");
        printf("  wifi                 Show Wi-Fi AP & BLE status\r\n");
        printf("  samples              Create hello/fib/gfx/sensor/game.c\r\n");
        printf("  bootsel              Reboot into USB BOOTSEL mode\r\n");
        console_puts("help: see serial\n", COLOR_YELLOW);
    } else if (strcmp(line, "ps") == 0) {
        const tcb_t *tasks = os_get_tasks();
        uint32_t count = os_get_task_count();
        printf("ID  NAME            STATE  TICKS\r\n");
        console_puts("ID NAME      ST  TICKS\n", COLOR_GREEN);
        for (uint32_t i = 0; i < count; i++) {
            printf("%-3lu %-15s %-6s %lu\r\n",
                   (unsigned long)tasks[i].id,
                   tasks[i].name,
                   state_name(tasks[i].state),
                   (unsigned long)tasks[i].cpu_ticks);
            console_printf(COLOR_WHITE, "%lu  %-9s %-3s %lu\n",
                           (unsigned long)tasks[i].id,
                           tasks[i].name,
                           state_name(tasks[i].state),
                           (unsigned long)tasks[i].cpu_ticks);
        }
    } else if (strcmp(line, "uptime") == 0) {
        uint32_t ticks = os_get_ticks();
        printf("Uptime: %lu ms (%lu s)\r\n", (unsigned long)ticks, (unsigned long)(ticks / 1000));
        console_printf(COLOR_WHITE, "Up: %lus (%lums)\n",
                       (unsigned long)(ticks / 1000), (unsigned long)ticks);
    } else if (strcmp(line, "clear") == 0) {
        console_set_statusbar_active(true);
        console_clear();
        console_redraw_all();
        printf("TFT cleared.\r\n");
    } else if (strncmp(line, "echo ", 5) == 0) {
        printf("%s\r\n", line + 5);
        console_printf(COLOR_WHITE, "%s\n", line + 5);
    } else if (strncmp(line, "bl ", 3) == 0) {
        int val = atoi(line + 3);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        st7735_set_backlight((uint8_t)val);
        printf("Backlight: %d%%\r\n", val);
        console_printf(COLOR_GREEN, "BL -> %d%%\n", val);
    } else if (strcmp(line, "led on") == 0) {
        g_led_auto_blink = false;
        set_onboard_led(true);
        printf("LED ON\r\n");
        console_puts("LED: ON\n", COLOR_GREEN);
    } else if (strcmp(line, "led off") == 0) {
        g_led_auto_blink = false;
        set_onboard_led(false);
        printf("LED OFF\r\n");
        console_puts("LED: OFF\n", COLOR_YELLOW);
    } else if (strcmp(line, "led auto") == 0) {
        g_led_auto_blink = true;
        printf("LED AUTO BLINK\r\n");
        console_puts("LED: AUTO\n", COLOR_GREEN);
    } else if (strcmp(line, "ls") == 0) {
        static fs_file_info_t list[FS_MAX_FILES];
        int n = fs_list(list, FS_MAX_FILES);
        if (n == 0) {
            printf("(empty)\r\n");
            console_puts("(empty)\n", COLOR_YELLOW);
        } else {
            for (int i = 0; i < n; i++) {
                printf("  %-24s %lu B\r\n", list[i].name, (unsigned long)list[i].size);
                console_printf(COLOR_WHITE, "%-16s %luB\n", list[i].name, (unsigned long)list[i].size);
            }
        }
    } else if (strncmp(line, "cat ", 4) == 0) {
        char *name = skip_spaces(line + 4);
        static char fbuf[FS_MAX_FILESIZE + 1];
        int sz = fs_read(name, fbuf, FS_MAX_FILESIZE);
        if (sz < 0) {
            printf("File not found: %s\r\n", name);
            console_printf(COLOR_RED, "No such file: %s\n", name);
        } else {
            int n = sz > FS_MAX_FILESIZE ? FS_MAX_FILESIZE : sz;
            fbuf[n] = '\0';
            for (int i = 0; i < n; i++) {
                if (fbuf[i] == '\n') putchar('\r');
                putchar(fbuf[i]);
            }
            printf("\r\n");
            console_printf(COLOR_WHITE, "%s\n", fbuf);
        }
    } else if (strncmp(line, "write ", 6) == 0) {
        char *rest = NULL;
        char *name = split_arg(line + 6, &rest);
        if (!name) {
            printf("Usage: write <file> <text>\r\n");
        } else {
            int rc = fs_write(name, rest, strlen(rest));
            if (rc >= 0) {
                printf("Wrote %d bytes to %s\r\n", rc, name);
                console_printf(COLOR_GREEN, "Saved %s (%dB)\n", name, rc);
            } else {
                printf("Write error (%d)\r\n", rc);
                console_printf(COLOR_RED, "Write err %d\n", rc);
            }
        }
    } else if (strncmp(line, "append ", 7) == 0) {
        char *rest = NULL;
        char *name = split_arg(line + 7, &rest);
        if (!name) {
            printf("Usage: append <file> <text>\r\n");
        } else {
            static char linebuf[260];
            snprintf(linebuf, sizeof(linebuf), "\n%s", rest);
            int rc = fs_append(name, linebuf, strlen(linebuf));
            if (rc >= 0) {
                printf("Appended to %s (total %d bytes)\r\n", name, rc);
                console_printf(COLOR_GREEN, "Appended %s (%dB)\n", name, rc);
            } else {
                printf("Append error (%d)\r\n", rc);
                console_printf(COLOR_RED, "Append err %d\n", rc);
            }
        }
    } else if (strncmp(line, "rm ", 3) == 0) {
        char *name = skip_spaces(line + 3);
        int rc = fs_delete(name);
        if (rc == 0) {
            printf("Deleted %s\r\n", name);
            console_printf(COLOR_YELLOW, "Deleted %s\n", name);
        } else {
            printf("File not found: %s\r\n", name);
            console_printf(COLOR_RED, "No such file: %s\n", name);
        }
    } else if (strcmp(line, "df") == 0) {
        uint32_t uf, tf, ub, tb;
        fs_stat(&uf, &tf, &ub, &tb);
        printf("PicoFS: %lu/%lu files, %lu/%lu bytes used\r\n",
               (unsigned long)uf, (unsigned long)tf,
               (unsigned long)ub, (unsigned long)tb);
        console_printf(COLOR_WHITE, "FS: %lu/%lu files %luB\n",
                       (unsigned long)uf, (unsigned long)tf, (unsigned long)ub);
    } else if (strcmp(line, "format") == 0) {
        fs_format();
        printf("PicoFS formatted.\r\n");
        console_puts("PicoFS formatted\n", COLOR_YELLOW);
    } else if (strcmp(line, "vi") == 0 || strncmp(line, "vi ", 3) == 0) {
        char *name = (strlen(line) > 3) ? skip_spaces(line + 3) : "";
        vi_main(name);
    } else if (strcmp(line, "cc") == 0 || strncmp(line, "cc ", 3) == 0) {
        char *args = (strlen(line) > 3) ? line + 3 : "";
        handle_cc_command(args);
    } else if (strcmp(line, "run") == 0 || strncmp(line, "run ", 4) == 0) {
        char *bin = (strlen(line) > 4) ? skip_spaces(line + 4) : "a.out";
        cc_run_file(bin);
    } else if (strncmp(line, "./", 2) == 0) {
        char *bin = skip_spaces(line + 2);
        cc_run_file(bin);
    } else if (strcmp(line, "disasm") == 0 || strncmp(line, "disasm ", 7) == 0) {
        char *bin = (strlen(line) > 7) ? skip_spaces(line + 7) : "a.out";
        cc_disasm_file(bin);
    } else if (strcmp(line, "rx") == 0 || strncmp(line, "rx ", 3) == 0) {
        char *name = (strlen(line) > 3) ? skip_spaces(line + 3) : "";
        transfer_rx_file(name);
    } else if (strcmp(line, "tx") == 0 || strncmp(line, "tx ", 3) == 0) {
        char *name = (strlen(line) > 3) ? skip_spaces(line + 3) : "";
        transfer_tx_file(name);
    } else if (strcmp(line, "pinout") == 0) {
        handle_pinout_command();
    } else if (strcmp(line, "disp") == 0 || strncmp(line, "disp ", 5) == 0) {
        char *args = (strlen(line) > 5) ? line + 5 : "";
        handle_disp_command(args);
    } else if (strcmp(line, "gpio") == 0 || strncmp(line, "gpio ", 5) == 0) {
        char *args = (strlen(line) > 5) ? line + 5 : "";
        handle_gpio_command(args);
    } else if (strcmp(line, "i2c") == 0 || strncmp(line, "i2c ", 4) == 0) {
        char *args = (strlen(line) > 4) ? line + 4 : "";
        handle_i2c_command(args);
    } else if (strcmp(line, "ble") == 0) {
        ble_uart_print_status();
        console_printf(COLOR_GREEN, "BLE: %s (%s)\n",
                       BLE_DEVICE_NAME,
                       ble_uart_is_connected() ? "CONN" : "ADV");
    } else if (strcmp(line, "wifi") == 0) {
        wifi_ap_print_status();
        ble_uart_print_status();
        console_printf(COLOR_CYAN, "WiFi: %s (%s)\n", WIFI_AP_SSID, WIFI_AP_IP);
    } else if (strcmp(line, "samples") == 0) {
        shell_seed_samples();
        printf("Created sample files: hello.c, fib.c, gfx.c, sensor.c, game.c\r\n");
        console_puts("Seeded 5 samples\n", COLOR_GREEN);
    } else if (strcmp(line, "bootsel") == 0) {
        printf("Rebooting into BOOTSEL...\r\n");
        console_puts("Rebooting BOOTSEL...\n", COLOR_ORANGE);
        os_sleep_ms(100);
        reset_usb_boot(0, 0);
    } else {
        printf("Unknown command: %s (type 'help')\r\n", line);
        console_printf(COLOR_RED, "Err: %s\n", line);
    }
}

void shell_task(void *arg) {
    (void)arg;
    char buf[256];
    size_t len = 0;
    bool banner_shown = false;

    while (1) {
        if (!banner_shown && stdio_usb_connected()) {
            banner_shown = true;
            printf("\r\n=== PicoOS v0.7 (RP2040 + PicoFS + vi + cc + BLE + Wi-Fi) ===\r\n");
            printf("BLE UART: Name=%s (auto-connect via app)\r\n", BLE_DEVICE_NAME);
            printf("Wi-Fi AP: SSID=%s PASS=%s URL=http://%s\r\n",
                   WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_IP);
            printf("Type 'help' for commands.\r\n");
            printf("pico-os> ");
        }

        int ch = os_getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            if (ch == '\r' || ch == '\n') {
                printf("\r\n");
                buf[len] = '\0';
                exec_command(buf);
                len = 0;
                printf("pico-os> ");
                fflush(stdout);
            } else if (ch == '\b' || ch == 127) {
                if (len > 0) {
                    len--;
                    buf[len] = '\0';
                    printf("\b \b");
                    fflush(stdout);
                    console_preview_input("> ", buf);
                }
            } else if (ch >= 0x20 && ch < 0x7F && len < sizeof(buf) - 1) {
                buf[len++] = (char)ch;
                buf[len] = '\0';
                putchar(ch);
                fflush(stdout);
                console_preview_input("> ", buf);
            }
        } else {
            os_sleep_ms(10);
        }
    }
}
