#include "vi.h"
#include "../kernel/kernel.h"
#include "../drivers/st7735.h"
#include "../ui/console.h"
#include "../fs/fs.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define VI_MAX_LINES     100
#define VI_MAX_COLS      76
#define VI_TERM_ROWS     20
#define VI_TERM_COLS     76
#define VI_TFT_ROWS      10
#define VI_TFT_COLS      26

typedef enum {
    MODE_NORMAL = 0,
    MODE_INSERT,
    MODE_COMMAND
} vi_mode_t;

enum {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_DEL,
    KEY_ESC = 27
};

static char lines[VI_MAX_LINES][VI_MAX_COLS];
static char save_buf[FS_MAX_FILESIZE];
static char cur_filename[FS_MAX_FILENAME];
static char cmd_buf[48];
static char status_msg[64];

static int line_count = 1;
static int cx = 0;
static int cy = 0;
static int view_top = 0;
static int tft_top = 0;
static bool dirty = false;
static vi_mode_t mode = MODE_NORMAL;
static char pending_op = 0;

static void clamp_cursor(void) {
    if (cy < 0) cy = 0;
    if (cy >= line_count) cy = line_count - 1;
    int len = (int)strlen(lines[cy]);
    int max_x = (mode == MODE_INSERT) ? len : (len > 0 ? len - 1 : 0);
    if (cx < 0) cx = 0;
    if (cx > max_x) cx = max_x;
}

static void adjust_scroll(void) {
    if (cy < view_top) view_top = cy;
    if (cy >= view_top + VI_TERM_ROWS) view_top = cy - VI_TERM_ROWS + 1;
    if (view_top < 0) view_top = 0;

    if (cy < tft_top) tft_top = cy;
    if (cy >= tft_top + VI_TFT_ROWS) tft_top = cy - VI_TFT_ROWS + 1;
    if (tft_top < 0) tft_top = 0;
}

static void load_file(const char *filename) {
    memset(lines, 0, sizeof(lines));
    strncpy(cur_filename, filename, FS_MAX_FILENAME - 1);
    cur_filename[FS_MAX_FILENAME - 1] = '\0';

    static char read_buf[FS_MAX_FILESIZE + 1];
    int sz = fs_read(cur_filename, read_buf, FS_MAX_FILESIZE);
    if (sz < 0) {
        line_count = 1;
        lines[0][0] = '\0';
        dirty = false;
        snprintf(status_msg, sizeof(status_msg), "[New File]");
        return;
    }

    int n = (sz > FS_MAX_FILESIZE) ? FS_MAX_FILESIZE : sz;
    read_buf[n] = '\0';

    line_count = 0;
    int col = 0;
    for (int i = 0; i < n; i++) {
        char c = read_buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            lines[line_count][col] = '\0';
            if (line_count < VI_MAX_LINES - 1) {
                line_count++;
                col = 0;
            } else {
                break;
            }
        } else {
            if (col < VI_MAX_COLS - 1) {
                lines[line_count][col++] = c;
            }
        }
    }
    lines[line_count][col] = '\0';
    line_count++;
    if (line_count > VI_MAX_LINES) line_count = VI_MAX_LINES;
    dirty = false;
    snprintf(status_msg, sizeof(status_msg), "\"%s\" %dL, %dB", cur_filename, line_count, n);
}

static bool save_file(const char *target_name) {
    if (target_name && target_name[0] != '\0') {
        strncpy(cur_filename, target_name, FS_MAX_FILENAME - 1);
        cur_filename[FS_MAX_FILENAME - 1] = '\0';
    }

    size_t pos = 0;
    for (int i = 0; i < line_count; i++) {
        size_t len = strlen(lines[i]);
        if (pos + len + (i + 1 < line_count ? 1U : 0U) > FS_MAX_FILESIZE) {
            snprintf(status_msg, sizeof(status_msg), "Error: exceeds %dB limit", FS_MAX_FILESIZE);
            return false;
        }
        if (len > 0) {
            memcpy(save_buf + pos, lines[i], len);
            pos += len;
        }
        if (i + 1 < line_count) {
            save_buf[pos++] = '\n';
        }
    }

    int rc = fs_write(cur_filename, save_buf, pos);
    if (rc < 0) {
        snprintf(status_msg, sizeof(status_msg), "Write error (%d)", rc);
        return false;
    }
    dirty = false;
    snprintf(status_msg, sizeof(status_msg), "\"%s\" %dL, %uB written",
             cur_filename, line_count, (unsigned)pos);
    return true;
}

static const char *mode_label_term(void) {
    switch (mode) {
        case MODE_INSERT:  return "-- INSERT --";
        case MODE_COMMAND: return "-- COMMAND --";
        default:           return "-- NORMAL --";
    }
}

static void render_term(void) {
    printf("\x1b[?25l\x1b[H");
    for (int r = 0; r < VI_TERM_ROWS; r++) {
        int line_idx = view_top + r;
        printf("\x1b[2K");
        if (line_idx < line_count) {
            printf("%.*s", VI_TERM_COLS, lines[line_idx]);
        } else {
            printf("\x1b[34m~\x1b[0m");
        }
        printf("\r\n");
    }

    /* Row 21: Inverted status bar */
    printf("\x1b[2K\x1b[7m %-13s  %s%s   Ln %d/%d Col %d \x1b[0m\r\n",
           mode_label_term(),
           cur_filename,
           dirty ? " [+]" : "",
           cy + 1, line_count, cx + 1);

    /* Row 22: Command or status message */
    printf("\x1b[2K");
    if (mode == MODE_COMMAND) {
        printf(":%s", cmd_buf);
        int cur_col = 2 + (int)strlen(cmd_buf);
        printf("\x1b[22;%dH\x1b[?25h", cur_col);
    } else {
        printf("%s", status_msg);
        int scr_row = (cy - view_top) + 1;
        int scr_col = cx + 1;
        if (scr_row < 1) scr_row = 1;
        if (scr_row > VI_TERM_ROWS) scr_row = VI_TERM_ROWS;
        if (scr_col < 1) scr_col = 1;
        if (scr_col > VI_TERM_COLS) scr_col = VI_TERM_COLS;
        printf("\x1b[%d;%dH\x1b[?25h", scr_row, scr_col);
    }
    fflush(stdout);
}

static void render_tft(void) {
    /* Header bar */
    st7735_fill_rect(0, 0, ST7735_WIDTH, 14, COLOR_NAVY);
    st7735_draw_hline(0, 14, ST7735_WIDTH, COLOR_TEAL);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "VI:%.15s%s", cur_filename, dirty ? "*" : "");
    st7735_draw_string(2, 3, hdr, COLOR_CYAN, COLOR_NAVY, 1);

    const char *mb = (mode == MODE_INSERT) ? "INS" :
                     (mode == MODE_COMMAND) ? "CMD" : "NRM";
    uint16_t mb_bg = (mode == MODE_INSERT) ? COLOR_GREEN :
                     (mode == MODE_COMMAND) ? COLOR_ORANGE : COLOR_TEAL;
    uint16_t mb_fg = (mode == MODE_INSERT || mode == MODE_COMMAND) ? COLOR_BLACK : COLOR_WHITE;
    st7735_fill_rect(136, 1, 22, 11, mb_bg);
    st7735_draw_string(138, 3, mb, mb_fg, mb_bg, 1);

    /* Horizontal pan for TFT 26-col window */
    int hscroll = 0;
    if (cx >= VI_TFT_COLS - 2) {
        hscroll = cx - (VI_TFT_COLS - 3);
    }

    /* 10 visible text rows at y = 16..115 */
    for (int r = 0; r < VI_TFT_ROWS; r++) {
        uint16_t y = (uint16_t)(16 + r * 10);
        int idx = tft_top + r;
        bool is_cur = (idx == cy);
        uint16_t row_bg = is_cur ? COLOR_NAVY : COLOR_BLACK;
        st7735_fill_rect(0, y, ST7735_WIDTH, 10, row_bg);

        if (idx < line_count) {
            const char *s = lines[idx];
            int slen = (int)strlen(s);
            char vis[VI_TFT_COLS + 1];
            int vlen = 0;
            if (hscroll < slen) {
                while (vlen < VI_TFT_COLS && (hscroll + vlen) < slen) {
                    vis[vlen] = s[hscroll + vlen];
                    vlen++;
                }
            }
            vis[vlen] = '\0';
            if (vlen > 0) {
                st7735_draw_string(2, y + 1, vis, COLOR_WHITE, row_bg, 1);
            }
            if (is_cur && mode != MODE_COMMAND) {
                int ccol = cx - hscroll;
                if (ccol >= 0 && ccol < VI_TFT_COLS) {
                    char ch = (cx < slen) ? s[cx] : ' ';
                    uint16_t cur_bg = (mode == MODE_INSERT) ? COLOR_GREEN : COLOR_YELLOW;
                    st7735_draw_char((uint16_t)(2 + ccol * 6), y + 1, ch, COLOR_BLACK, cur_bg, 1);
                }
            }
        } else {
            st7735_draw_char(2, y + 1, '~', COLOR_TEAL, COLOR_BLACK, 1);
        }
    }

    /* Bottom status bar at y = 116..127 */
    st7735_fill_rect(0, 116, ST7735_WIDTH, 12, COLOR_DARKGRAY);
    char foot[VI_TFT_COLS + 1];
    if (mode == MODE_COMMAND) {
        snprintf(foot, sizeof(foot), ":%s_", cmd_buf);
        st7735_draw_string(2, 118, foot, COLOR_YELLOW, COLOR_DARKGRAY, 1);
    } else if (status_msg[0] != '\0') {
        snprintf(foot, sizeof(foot), "%.*s", VI_TFT_COLS, status_msg);
        st7735_draw_string(2, 118, foot, COLOR_WHITE, COLOR_DARKGRAY, 1);
    } else {
        snprintf(foot, sizeof(foot), "L%d/%d C%d", cy + 1, line_count, cx + 1);
        st7735_draw_string(2, 118, foot, COLOR_WHITE, COLOR_DARKGRAY, 1);
    }
}

static void render_all(void) {
    adjust_scroll();
    render_term();
    render_tft();
}

static int read_key(void) {
    int ch = getchar_timeout_us(20000);
    if (ch == PICO_ERROR_TIMEOUT) {
        return -1;
    }
    if (ch == KEY_ESC) {
        int c1 = getchar_timeout_us(25000);
        if (c1 == PICO_ERROR_TIMEOUT) {
            return KEY_ESC;
        }
        if (c1 == '[') {
            int c2 = getchar_timeout_us(25000);
            if (c2 == PICO_ERROR_TIMEOUT) return KEY_ESC;
            switch (c2) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                case '1':
                case '3':
                case '4': {
                    int c3 = getchar_timeout_us(25000);
                    (void)c3;
                    if (c2 == '1') return KEY_HOME;
                    if (c2 == '3') return KEY_DEL;
                    return KEY_END;
                }
                default: return -1;
            }
        }
        return KEY_ESC;
    }
    return ch;
}

static void insert_empty_line_at(int row) {
    if (line_count >= VI_MAX_LINES) return;
    for (int i = line_count; i > row; i--) {
        memcpy(lines[i], lines[i - 1], VI_MAX_COLS);
    }
    lines[row][0] = '\0';
    line_count++;
    dirty = true;
}

static void delete_line_at(int row) {
    if (line_count <= 1) {
        lines[0][0] = '\0';
        cx = 0;
        cy = 0;
        dirty = true;
        return;
    }
    for (int i = row; i < line_count - 1; i++) {
        memcpy(lines[i], lines[i + 1], VI_MAX_COLS);
    }
    lines[line_count - 1][0] = '\0';
    line_count--;
    if (cy >= line_count) cy = line_count - 1;
    dirty = true;
}

static void insert_char_at_cursor(char c) {
    int len = (int)strlen(lines[cy]);
    if (len >= VI_MAX_COLS - 1) return;
    memmove(&lines[cy][cx + 1], &lines[cy][cx], (size_t)(len - cx + 1));
    lines[cy][cx] = c;
    cx++;
    dirty = true;
}

static void delete_char_at(int row, int col) {
    int len = (int)strlen(lines[row]);
    if (col < 0 || col >= len) return;
    memmove(&lines[row][col], &lines[row][col + 1], (size_t)(len - col));
    dirty = true;
}

static bool handle_command_mode(int key, bool *quit_flag) {
    if (key == KEY_ESC) {
        mode = MODE_NORMAL;
        cmd_buf[0] = '\0';
        status_msg[0] = '\0';
        return true;
    }
    if (key == '\r' || key == '\n') {
        char *cmd = cmd_buf;
        while (*cmd == ' ') cmd++;

        if (strcmp(cmd, "q") == 0) {
            if (dirty) {
                snprintf(status_msg, sizeof(status_msg), "Unsaved changes! Use :q! or :wq");
            } else {
                *quit_flag = true;
            }
        } else if (strcmp(cmd, "q!") == 0) {
            *quit_flag = true;
        } else if (strcmp(cmd, "w") == 0) {
            save_file(NULL);
        } else if (strncmp(cmd, "w ", 2) == 0) {
            char *fn = cmd + 2;
            while (*fn == ' ') fn++;
            save_file(fn);
        } else if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
            if (save_file(NULL)) {
                *quit_flag = true;
            }
        } else if (cmd[0] != '\0') {
            snprintf(status_msg, sizeof(status_msg), "Not an editor command: %s", cmd);
        }
        mode = MODE_NORMAL;
        cmd_buf[0] = '\0';
        clamp_cursor();
        return true;
    }
    if (key == '\b' || key == 127) {
        size_t len = strlen(cmd_buf);
        if (len > 0) {
            cmd_buf[len - 1] = '\0';
        } else {
            mode = MODE_NORMAL;
        }
        return true;
    }
    if (key >= 0x20 && key < 0x7F) {
        size_t len = strlen(cmd_buf);
        if (len < sizeof(cmd_buf) - 1) {
            cmd_buf[len] = (char)key;
            cmd_buf[len + 1] = '\0';
        }
        return true;
    }
    return false;
}

static bool handle_insert_mode(int key) {
    if (key == KEY_ESC) {
        mode = MODE_NORMAL;
        if (cx > 0) cx--;
        clamp_cursor();
        status_msg[0] = '\0';
        return true;
    }
    if (key == KEY_UP) {
        cy--;
        clamp_cursor();
        return true;
    }
    if (key == KEY_DOWN) {
        cy++;
        clamp_cursor();
        return true;
    }
    if (key == KEY_LEFT) {
        cx--;
        clamp_cursor();
        return true;
    }
    if (key == KEY_RIGHT) {
        cx++;
        clamp_cursor();
        return true;
    }
    if (key == KEY_HOME) {
        cx = 0;
        return true;
    }
    if (key == KEY_END) {
        cx = (int)strlen(lines[cy]);
        return true;
    }
    if (key == KEY_DEL) {
        int len = (int)strlen(lines[cy]);
        if (cx < len) {
            delete_char_at(cy, cx);
        } else if (cy + 1 < line_count) {
            int next_len = (int)strlen(lines[cy + 1]);
            if (len + next_len < VI_MAX_COLS) {
                strcat(lines[cy], lines[cy + 1]);
                delete_line_at(cy + 1);
            }
        }
        return true;
    }
    if (key == '\r' || key == '\n') {
        if (line_count >= VI_MAX_LINES) return false;
        char tail[VI_MAX_COLS];
        strncpy(tail, &lines[cy][cx], VI_MAX_COLS - 1);
        tail[VI_MAX_COLS - 1] = '\0';
        lines[cy][cx] = '\0';
        insert_empty_line_at(cy + 1);
        cy++;
        strncpy(lines[cy], tail, VI_MAX_COLS - 1);
        lines[cy][VI_MAX_COLS - 1] = '\0';
        cx = 0;
        return true;
    }
    if (key == '\b' || key == 127) {
        if (cx > 0) {
            cx--;
            delete_char_at(cy, cx);
        } else if (cy > 0) {
            int prev_len = (int)strlen(lines[cy - 1]);
            int cur_len = (int)strlen(lines[cy]);
            if (prev_len + cur_len < VI_MAX_COLS) {
                strcat(lines[cy - 1], lines[cy]);
                delete_line_at(cy);
                cy--;
                cx = prev_len;
            }
        }
        return true;
    }
    if (key == '\t') {
        insert_char_at_cursor(' ');
        insert_char_at_cursor(' ');
        return true;
    }
    if (key >= 0x20 && key < 0x7F) {
        insert_char_at_cursor((char)key);
        return true;
    }
    return false;
}

static bool handle_normal_mode(int key) {
    if (pending_op == 'd') {
        pending_op = 0;
        if (key == 'd') {
            delete_line_at(cy);
            clamp_cursor();
            snprintf(status_msg, sizeof(status_msg), "1 line deleted");
            return true;
        }
        return false;
    }
    if (pending_op == 'g') {
        pending_op = 0;
        if (key == 'g') {
            cy = 0;
            cx = 0;
            return true;
        }
        return false;
    }

    switch (key) {
        case 'h':
        case KEY_LEFT:
            cx--;
            clamp_cursor();
            return true;
        case 'l':
        case KEY_RIGHT:
            cx++;
            clamp_cursor();
            return true;
        case 'k':
        case KEY_UP:
            cy--;
            clamp_cursor();
            return true;
        case 'j':
        case KEY_DOWN:
            cy++;
            clamp_cursor();
            return true;
        case '0':
        case KEY_HOME:
            cx = 0;
            return true;
        case '$':
        case KEY_END: {
            int len = (int)strlen(lines[cy]);
            cx = (len > 0) ? len - 1 : 0;
            return true;
        }
        case 'G':
            cy = line_count - 1;
            clamp_cursor();
            return true;
        case 'g':
            pending_op = 'g';
            return false;
        case 'd':
            pending_op = 'd';
            return false;
        case 'x':
        case KEY_DEL:
            delete_char_at(cy, cx);
            clamp_cursor();
            return true;
        case 'i':
            mode = MODE_INSERT;
            status_msg[0] = '\0';
            return true;
        case 'I':
            mode = MODE_INSERT;
            cx = 0;
            status_msg[0] = '\0';
            return true;
        case 'a':
            mode = MODE_INSERT;
            if (lines[cy][0] != '\0') cx++;
            clamp_cursor();
            status_msg[0] = '\0';
            return true;
        case 'A':
            mode = MODE_INSERT;
            cx = (int)strlen(lines[cy]);
            status_msg[0] = '\0';
            return true;
        case 'o':
            if (line_count < VI_MAX_LINES) {
                insert_empty_line_at(cy + 1);
                cy++;
                cx = 0;
                mode = MODE_INSERT;
                status_msg[0] = '\0';
            }
            return true;
        case 'O':
            if (line_count < VI_MAX_LINES) {
                insert_empty_line_at(cy);
                cx = 0;
                mode = MODE_INSERT;
                status_msg[0] = '\0';
            }
            return true;
        case ':':
            mode = MODE_COMMAND;
            cmd_buf[0] = '\0';
            status_msg[0] = '\0';
            return true;
        default:
            return false;
    }
}

void vi_main(const char *filename) {
    if (!filename || filename[0] == '\0') {
        printf("Usage: vi <filename>\r\n");
        return;
    }

    cx = 0;
    cy = 0;
    view_top = 0;
    tft_top = 0;
    mode = MODE_NORMAL;
    pending_op = 0;
    cmd_buf[0] = '\0';
    status_msg[0] = '\0';

    load_file(filename);
    console_set_statusbar_active(false);
    printf("\x1b[2J\x1b[H");
    render_all();

    bool quit_flag = false;
    while (!quit_flag) {
        int key = read_key();
        if (key < 0) {
            os_sleep_ms(5);
            continue;
        }
        bool need_redraw = false;
        if (mode == MODE_COMMAND) {
            need_redraw = handle_command_mode(key, &quit_flag);
        } else if (mode == MODE_INSERT) {
            need_redraw = handle_insert_mode(key);
        } else {
            need_redraw = handle_normal_mode(key);
        }
        if (!quit_flag && need_redraw) {
            render_all();
        }
    }

    printf("\x1b[2J\x1b[H\x1b[?25h");
    fflush(stdout);
    console_set_statusbar_active(true);
    console_redraw_all();
}
