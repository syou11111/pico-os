#include "shell.h"
#include "../kernel/kernel.h"
#include "../drivers/st7735.h"
#include "../ui/console.h"
#include "../fs/fs.h"
#include "../apps/vi.h"
#include "../apps/cc.h"
#include "../transfer/transfer.h"
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

    fs_write("hello.c", hello_c, strlen(hello_c));
    fs_write("fib.c", fib_c, strlen(fib_c));
    fs_write("gfx.c", gfx_c, strlen(gfx_c));
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
        printf("  samples              Create hello.c, fib.c, gfx.c\r\n");
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
    } else if (strcmp(line, "samples") == 0) {
        shell_seed_samples();
        printf("Created sample files: hello.c, fib.c, gfx.c\r\n");
        console_puts("Seeded hello/fib/gfx.c\n", COLOR_GREEN);
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
            printf("\r\n=== PicoOS v0.4 (RP2040 + PicoFS + vi + cc + rx/tx) ===\r\n");
            printf("Type 'help' for commands.\r\n");
            printf("pico-os> ");
        }

        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            if (ch == '\r' || ch == '\n') {
                printf("\r\n");
                buf[len] = '\0';
                exec_command(buf);
                len = 0;
                printf("pico-os> ");
            } else if (ch == '\b' || ch == 127) {
                if (len > 0) {
                    len--;
                    printf("\b \b");
                }
            } else if (ch >= 0x20 && ch < 0x7F && len < sizeof(buf) - 1) {
                buf[len++] = (char)ch;
                putchar(ch);
            }
        } else {
            os_sleep_ms(10);
        }
    }
}
