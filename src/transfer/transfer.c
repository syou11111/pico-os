#include "transfer.h"
#include "../kernel/kernel.h"
#include "../drivers/st7735.h"
#include "../ui/console.h"
#include "../fs/fs.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

static uint8_t xfer_buf[FS_MAX_FILESIZE];

static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static int hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_byte_timeout(uint32_t timeout_ms) {
    uint32_t start = os_get_ticks();
    while (1) {
        int ch = getchar_timeout_us(0);
        if (ch != PICO_ERROR_TIMEOUT) {
            return ch;
        }
        if ((os_get_ticks() - start) >= timeout_ms) {
            return -1;
        }
        os_sleep_ms(2);
    }
}

static int read_line_rest(char *buf, size_t max_len, uint32_t timeout_ms) {
    size_t len = 0;
    while (1) {
        int ch = read_byte_timeout(timeout_ms);
        if (ch < 0) return -1;
        if (ch == 0x03) return -2;
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r') {
                int nxt = getchar_timeout_us(5000);
                (void)nxt;
            }
            buf[len] = '\0';
            return (int)len;
        }
        if (len < max_len - 1) {
            buf[len++] = (char)ch;
        }
    }
}

int transfer_tx_file(const char *filename) {
    if (!filename || filename[0] == '\0' || strlen(filename) >= FS_MAX_FILENAME) {
        printf("\r\n@ERR invalid_filename\r\n");
        fflush(stdout);
        return -1;
    }

    int sz = fs_read(filename, xfer_buf, FS_MAX_FILESIZE);
    if (sz < 0) {
        printf("\r\n@ERR file_not_found\r\n");
        fflush(stdout);
        console_printf(COLOR_RED, "TX err: %s\n", filename);
        return -1;
    }

    uint16_t crc = crc16_ccitt(xfer_buf, (size_t)sz);
    printf("\r\n@BEGIN %s %d %04X\r\n", filename, sz, (unsigned)crc);
    for (int i = 0; i < sz; i++) {
        printf("%02X", (unsigned)xfer_buf[i]);
        if ((i % 32) == 31 || i == sz - 1) {
            printf("\r\n");
        }
    }
    printf("@END %04X\r\n", (unsigned)crc);
    fflush(stdout);

    console_printf(COLOR_GREEN, "TX: %s (%dB) OK\n", filename, sz);
    return sz;
}

static int rx_framed_mode(const char *filename) {
    char line[64];
    int lrc = read_line_rest(line, sizeof(line), 3000);
    if (lrc < 0) {
        printf("\r\n@ERR header_timeout\r\n");
        fflush(stdout);
        return -1;
    }

    if (strncmp(line, "BEGIN ", 6) != 0) {
        printf("\r\n@ERR bad_header\r\n");
        fflush(stdout);
        return -1;
    }

    char *p = line + 6;
    while (*p == ' ') p++;
    char *endp = NULL;
    long expected_size = strtol(p, &endp, 10);
    if (endp == p || expected_size < 0 || expected_size > FS_MAX_FILESIZE) {
        printf("\r\n@ERR bad_size\r\n");
        fflush(stdout);
        return -1;
    }

    p = endp;
    while (*p == ' ') p++;
    unsigned long expected_crc = strtoul(p, &endp, 16);
    if (endp == p || expected_crc > 0xFFFFUL) {
        printf("\r\n@ERR bad_crc_hdr\r\n");
        fflush(stdout);
        return -1;
    }

    size_t decoded_len = 0;
    int hi_nibble = -1;
    bool saw_end = false;

    while (decoded_len < (size_t)expected_size) {
        int ch = read_byte_timeout(5000);
        if (ch < 0) {
            printf("\r\n@ERR data_timeout\r\n");
            fflush(stdout);
            return -1;
        }
        if (ch == 0x03) {
            printf("\r\n@ERR aborted\r\n");
            fflush(stdout);
            return -1;
        }
        if (ch == '@') {
            saw_end = true;
            break;
        }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            continue;
        }
        int hv = hex_val(ch);
        if (hv < 0) {
            printf("\r\n@ERR invalid_hex\r\n");
            fflush(stdout);
            return -1;
        }
        if (hi_nibble < 0) {
            hi_nibble = hv;
        } else {
            xfer_buf[decoded_len++] = (uint8_t)((hi_nibble << 4) | hv);
            hi_nibble = -1;
        }
    }

    if (hi_nibble >= 0) {
        printf("\r\n@ERR odd_hex\r\n");
        fflush(stdout);
        return -1;
    }

    /* Consume @END line cleanly */
    if (!saw_end) {
        while (1) {
            int ch = read_byte_timeout(2000);
            if (ch < 0) break;
            if (ch == '@') {
                saw_end = true;
                break;
            }
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                printf("\r\n@ERR size_mismatch\r\n");
                fflush(stdout);
                return -1;
            }
        }
    }
    if (saw_end) {
        char end_line[32];
        read_line_rest(end_line, sizeof(end_line), 1000);
    }

    if (decoded_len != (size_t)expected_size) {
        printf("\r\n@ERR size_mismatch (%u != %ld)\r\n",
               (unsigned)decoded_len, expected_size);
        fflush(stdout);
        console_puts("RX err: size\n", COLOR_RED);
        return -1;
    }

    uint16_t actual_crc = crc16_ccitt(xfer_buf, decoded_len);
    if (actual_crc != (uint16_t)expected_crc) {
        printf("\r\n@ERR crc_mismatch (%04X != %04lX)\r\n",
               (unsigned)actual_crc, expected_crc);
        fflush(stdout);
        console_puts("RX err: CRC\n", COLOR_RED);
        return -1;
    }

    int wrc = fs_write(filename, xfer_buf, decoded_len);
    if (wrc < 0) {
        printf("\r\n@ERR fs_write (%d)\r\n", wrc);
        fflush(stdout);
        console_printf(COLOR_RED, "RX err: FS %d\n", wrc);
        return -1;
    }

    printf("\r\n@OK %s %u %04X\r\n", filename, (unsigned)decoded_len, (unsigned)actual_crc);
    fflush(stdout);
    console_printf(COLOR_GREEN, "RX: %s (%uB) OK\n", filename, (unsigned)decoded_len);
    return (int)decoded_len;
}

static int rx_paste_mode(const char *filename, int first_ch) {
    size_t len = 0;
    int prev_ch = 0;
    int ch = first_ch;

    while (1) {
        if (ch == 0x04) { /* Ctrl+D: EOF */
            break;
        }
        if (ch == 0x03) { /* Ctrl+C: Abort */
            printf("\r\nAborted.\r\n");
            fflush(stdout);
            console_puts("RX aborted\n", COLOR_YELLOW);
            return -1;
        }
        if (ch == '\b' || ch == 127) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (ch == '\r') {
            if (len < FS_MAX_FILESIZE) {
                xfer_buf[len++] = '\n';
                printf("\r\n");
                fflush(stdout);
            }
        } else if (ch == '\n') {
            if (prev_ch != '\r' && len < FS_MAX_FILESIZE) {
                xfer_buf[len++] = '\n';
                printf("\r\n");
                fflush(stdout);
            }
        } else if ((ch >= 0x20 && ch < 0x7F) || ch == '\t') {
            if (len < FS_MAX_FILESIZE) {
                xfer_buf[len++] = (uint8_t)ch;
                putchar(ch);
                fflush(stdout);
            }
        }

        prev_ch = ch;
        ch = read_byte_timeout(60000);
        if (ch < 0) {
            printf("\r\nTimeout.\r\n");
            fflush(stdout);
            return -1;
        }
    }

    int wrc = fs_write(filename, xfer_buf, len);
    if (wrc < 0) {
        printf("\r\nWrite error (%d)\r\n", wrc);
        fflush(stdout);
        console_printf(COLOR_RED, "RX err: %d\n", wrc);
        return -1;
    }

    printf("\r\nSaved %s (%u bytes)\r\n", filename, (unsigned)len);
    fflush(stdout);
    console_printf(COLOR_GREEN, "RX: %s (%uB) OK\n", filename, (unsigned)len);
    return (int)len;
}

int transfer_rx_file(const char *filename) {
    if (!filename || filename[0] == '\0' || strlen(filename) >= FS_MAX_FILENAME) {
        printf("Usage: rx <filename>\r\n");
        return -1;
    }

    printf("\r\n@READY %s (Send @BEGIN <size> <crc> or paste text + Ctrl-D)\r\n", filename);
    fflush(stdout);
    console_printf(COLOR_CYAN, "RX wait: %s\n", filename);

    int first = -1;
    while (1) {
        int ch = read_byte_timeout(30000);
        if (ch < 0) {
            printf("\r\n@ERR timeout\r\n");
            fflush(stdout);
            console_puts("RX timeout\n", COLOR_YELLOW);
            return -1;
        }
        if (ch == 0x03) {
            printf("\r\n@ERR aborted\r\n");
            fflush(stdout);
            console_puts("RX aborted\n", COLOR_YELLOW);
            return -1;
        }
        if (ch == '\r' || ch == '\n') {
            continue;
        }
        first = ch;
        break;
    }

    if (first == '@') {
        return rx_framed_mode(filename);
    }
    return rx_paste_mode(filename, first);
}
