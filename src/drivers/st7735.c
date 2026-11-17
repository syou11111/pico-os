#include "st7735.h"
#include "font5x7.h"
#include "../kernel/kernel.h"
#include "../fs/fs.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include <string.h>

/* ST7735S Commands */
#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_NORON   0x13
#define CMD_INVOFF  0x20
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_FRMCTR1 0xB1
#define CMD_FRMCTR2 0xB2
#define CMD_FRMCTR3 0xB3
#define CMD_INVCTR  0xB4
#define CMD_PWCTR1  0xC0
#define CMD_PWCTR2  0xC1
#define CMD_PWCTR3  0xC2
#define CMD_PWCTR4  0xC3
#define CMD_PWCTR5  0xC4
#define CMD_VMCTR1  0xC5
#define CMD_GMCTRP1 0xE0
#define CMD_GMCTRN1 0xE1

#define DISP_CFG_FILE  ".disp.cfg"
#define DISP_CFG_MAGIC 0x44495350UL /* "DISP" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    st7735_pins_t pins;
} disp_cfg_t;

static st7735_pins_t cur_pins = {
    .spi_bus = 0,
    .cs   = ST7735_DEFAULT_CS,
    .sck  = ST7735_DEFAULT_SCK,
    .mosi = ST7735_DEFAULT_MOSI,
    .rst  = ST7735_DEFAULT_RST,
    .dc   = ST7735_DEFAULT_DC,
    .blk  = ST7735_DEFAULT_BLK
};

static volatile bool spi_locked = false;
static uint bl_pwm_slice = 0;
static uint bl_pwm_chan = 0;
static bool display_initialized = false;

static inline spi_inst_t *active_spi(void) {
    return (cur_pins.spi_bus == 1) ? spi1 : spi0;
}

static void lock_spi(void) {
    while (true) {
        uint32_t primask = os_enter_critical();
        if (!spi_locked) {
            spi_locked = true;
            os_exit_critical(primask);
            return;
        }
        os_exit_critical(primask);
        os_yield();
    }
}

static void unlock_spi(void) {
    uint32_t primask = os_enter_critical();
    spi_locked = false;
    os_exit_critical(primask);
}

static inline void cs_select(void)   { gpio_put(cur_pins.cs, 0); }
static inline void cs_deselect(void) { gpio_put(cur_pins.cs, 1); }
static inline void dc_command(void)  { gpio_put(cur_pins.dc, 0); }
static inline void dc_data(void)     { gpio_put(cur_pins.dc, 1); }

static void write_cmd(uint8_t cmd) {
    dc_command();
    cs_select();
    spi_write_blocking(active_spi(), &cmd, 1);
    cs_deselect();
}

static void write_data(const uint8_t *data, size_t len) {
    if (len == 0) return;
    dc_data();
    cs_select();
    spi_write_blocking(active_spi(), data, len);
    cs_deselect();
}

static void write_data_byte(uint8_t val) {
    write_data(&val, 1);
}

static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t caset[4] = {
        (uint8_t)((x0 + ST7735_XSTART) >> 8), (uint8_t)((x0 + ST7735_XSTART) & 0xFF),
        (uint8_t)((x1 + ST7735_XSTART) >> 8), (uint8_t)((x1 + ST7735_XSTART) & 0xFF)
    };
    uint8_t raset[4] = {
        (uint8_t)((y0 + ST7735_YSTART) >> 8), (uint8_t)((y0 + ST7735_YSTART) & 0xFF),
        (uint8_t)((y1 + ST7735_YSTART) >> 8), (uint8_t)((y1 + ST7735_YSTART) & 0xFF)
    };

    write_cmd(CMD_CASET);
    write_data(caset, 4);
    write_cmd(CMD_RASET);
    write_data(raset, 4);
    write_cmd(CMD_RAMWR);
}

static bool is_valid_user_gpio(uint8_t pin) {
    return (pin <= 22) || (pin >= 26 && pin <= 28);
}

static int resolve_spi_bus(uint8_t sck, uint8_t mosi) {
    bool is_spi0_sck  = (sck == 2 || sck == 6 || sck == 18);
    bool is_spi0_mosi = (mosi == 3 || mosi == 7 || mosi == 19);
    if (is_spi0_sck && is_spi0_mosi) return 0;

    bool is_spi1_sck  = (sck == 10 || sck == 14);
    bool is_spi1_mosi = (mosi == 11 || mosi == 15);
    if (is_spi1_sck && is_spi1_mosi) return 1;

    return -1;
}

const st7735_pins_t *st7735_get_pins(void) {
    return &cur_pins;
}

bool st7735_is_display_pin(uint pin) {
    return pin == cur_pins.cs || pin == cur_pins.sck || pin == cur_pins.mosi ||
           pin == cur_pins.rst || pin == cur_pins.dc || pin == cur_pins.blk;
}

void st7735_load_saved_pins(void) {
    disp_cfg_t cfg;
    if (fs_read(DISP_CFG_FILE, &cfg, sizeof(cfg)) != (int)sizeof(cfg)) {
        return;
    }
    if (cfg.magic != DISP_CFG_MAGIC) return;

    const st7735_pins_t *p = &cfg.pins;
    uint8_t arr[6] = { p->cs, p->sck, p->mosi, p->rst, p->dc, p->blk };
    for (int i = 0; i < 6; i++) {
        if (!is_valid_user_gpio(arr[i])) return;
        for (int j = i + 1; j < 6; j++) {
            if (arr[i] == arr[j]) return;
        }
    }
    int bus = resolve_spi_bus(p->sck, p->mosi);
    if (bus < 0) return;

    cur_pins = *p;
    cur_pins.spi_bus = (uint8_t)bus;
}

int st7735_reconfigure_pins(uint8_t cs, uint8_t sck, uint8_t mosi,
                            uint8_t rst, uint8_t dc, uint8_t blk, bool persist) {
    uint8_t arr[6] = { cs, sck, mosi, rst, dc, blk };
    for (int i = 0; i < 6; i++) {
        if (!is_valid_user_gpio(arr[i])) return -1;
        for (int j = i + 1; j < 6; j++) {
            if (arr[i] == arr[j]) return -2;
        }
    }
    int bus = resolve_spi_bus(sck, mosi);
    if (bus < 0) return -3;

    lock_spi();
    if (display_initialized) {
        pwm_set_enabled(bl_pwm_slice, false);
        spi_deinit(active_spi());
        gpio_deinit(cur_pins.cs);
        gpio_deinit(cur_pins.sck);
        gpio_deinit(cur_pins.mosi);
        gpio_deinit(cur_pins.rst);
        gpio_deinit(cur_pins.dc);
        gpio_deinit(cur_pins.blk);
        display_initialized = false;
    }

    cur_pins.spi_bus = (uint8_t)bus;
    cur_pins.cs   = cs;
    cur_pins.sck  = sck;
    cur_pins.mosi = mosi;
    cur_pins.rst  = rst;
    cur_pins.dc   = dc;
    cur_pins.blk  = blk;
    unlock_spi();

    st7735_init();

    if (persist) {
        disp_cfg_t cfg = { .magic = DISP_CFG_MAGIC, .pins = cur_pins };
        if (fs_write(DISP_CFG_FILE, &cfg, sizeof(cfg)) < 0) {
            return -4;
        }
    }
    return 0;
}

int st7735_reset_pins(void) {
    fs_delete(DISP_CFG_FILE);
    return st7735_reconfigure_pins(
        ST7735_DEFAULT_CS, ST7735_DEFAULT_SCK, ST7735_DEFAULT_MOSI,
        ST7735_DEFAULT_RST, ST7735_DEFAULT_DC, ST7735_DEFAULT_BLK, false);
}

void st7735_set_backlight(uint8_t percent) {
    if (percent > 100) percent = 100;
    uint16_t level = (uint16_t)((percent * 255) / 100);
    pwm_set_chan_level(bl_pwm_slice, bl_pwm_chan, level);
}

void st7735_init(void) {
    /* Initialize SPI0 or SPI1 at 24 MHz */
    spi_init(active_spi(), 24 * 1000 * 1000);
    gpio_set_function(cur_pins.sck, GPIO_FUNC_SPI);
    gpio_set_function(cur_pins.mosi, GPIO_FUNC_SPI);

    /* Control pins */
    gpio_init(cur_pins.cs);
    gpio_set_dir(cur_pins.cs, GPIO_OUT);
    gpio_put(cur_pins.cs, 1);

    gpio_init(cur_pins.dc);
    gpio_set_dir(cur_pins.dc, GPIO_OUT);
    gpio_put(cur_pins.dc, 0);

    gpio_init(cur_pins.rst);
    gpio_set_dir(cur_pins.rst, GPIO_OUT);

    /* PWM Backlight */
    gpio_set_function(cur_pins.blk, GPIO_FUNC_PWM);
    bl_pwm_slice = pwm_gpio_to_slice_num(cur_pins.blk);
    bl_pwm_chan  = pwm_gpio_to_channel(cur_pins.blk);
    pwm_set_wrap(bl_pwm_slice, 255);
    pwm_set_enabled(bl_pwm_slice, true);
    st7735_set_backlight(85);

    /* Hardware reset */
    gpio_put(cur_pins.rst, 1);
    sleep_ms(10);
    gpio_put(cur_pins.rst, 0);
    sleep_ms(20);
    gpio_put(cur_pins.rst, 1);
    sleep_ms(120);

    write_cmd(CMD_SWRESET);
    sleep_ms(150);

    write_cmd(CMD_SLPOUT);
    sleep_ms(120);

    /* Frame rate control */
    static const uint8_t frm1[] = {0x01, 0x2C, 0x2D};
    static const uint8_t frm2[] = {0x01, 0x2C, 0x2D};
    static const uint8_t frm3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    write_cmd(CMD_FRMCTR1); write_data(frm1, sizeof(frm1));
    write_cmd(CMD_FRMCTR2); write_data(frm2, sizeof(frm2));
    write_cmd(CMD_FRMCTR3); write_data(frm3, sizeof(frm3));

    write_cmd(CMD_INVCTR);  write_data_byte(0x07);

    /* Power control */
    static const uint8_t pw1[] = {0xA2, 0x02, 0x84};
    static const uint8_t pw3[] = {0x0A, 0x00};
    static const uint8_t pw4[] = {0x8A, 0x2A};
    static const uint8_t pw5[] = {0x8A, 0xEE};
    write_cmd(CMD_PWCTR1); write_data(pw1, sizeof(pw1));
    write_cmd(CMD_PWCTR2); write_data_byte(0xC5);
    write_cmd(CMD_PWCTR3); write_data(pw3, sizeof(pw3));
    write_cmd(CMD_PWCTR4); write_data(pw4, sizeof(pw4));
    write_cmd(CMD_PWCTR5); write_data(pw5, sizeof(pw5));
    write_cmd(CMD_VMCTR1); write_data_byte(0x0E);

    write_cmd(CMD_INVOFF);

    /* Landscape orientation */
    write_cmd(CMD_MADCTL);
    write_data_byte(0xA0);

    /* 16-bit RGB565 color mode */
    write_cmd(CMD_COLMOD);
    write_data_byte(0x05);

    /* Gamma correction */
    static const uint8_t gm_p[] = {
        0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10
    };
    static const uint8_t gm_n[] = {
        0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10
    };
    write_cmd(CMD_GMCTRP1); write_data(gm_p, sizeof(gm_p));
    write_cmd(CMD_GMCTRN1); write_data(gm_n, sizeof(gm_n));

    write_cmd(CMD_NORON);
    sleep_ms(10);
    write_cmd(CMD_DISPON);
    sleep_ms(100);

    display_initialized = true;
    st7735_fill_screen(COLOR_BLACK);
}

void st7735_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (x >= ST7735_WIDTH || y >= ST7735_HEIGHT || w == 0 || h == 0) return;
    if (x + w > ST7735_WIDTH)  w = ST7735_WIDTH - x;
    if (y + h > ST7735_HEIGHT) h = ST7735_HEIGHT - y;

    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    uint8_t buf[128];
    for (size_t i = 0; i < sizeof(buf); i += 2) {
        buf[i]     = hi;
        buf[i + 1] = lo;
    }

    lock_spi();
    set_addr_window(x, y, x + w - 1, y + h - 1);
    dc_data();
    cs_select();

    uint32_t total_bytes = (uint32_t)w * (uint32_t)h * 2U;
    while (total_bytes > 0) {
        size_t chunk = total_bytes > sizeof(buf) ? sizeof(buf) : total_bytes;
        spi_write_blocking(active_spi(), buf, chunk);
        total_bytes -= chunk;
    }

    cs_deselect();
    unlock_spi();
}

void st7735_draw_pixel(uint16_t x, uint16_t y, uint16_t color) {
    if (x >= ST7735_WIDTH || y >= ST7735_HEIGHT) return;
    uint8_t pix[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };

    lock_spi();
    set_addr_window(x, y, x, y);
    dc_data();
    cs_select();
    spi_write_blocking(active_spi(), pix, 2);
    cs_deselect();
    unlock_spi();
}

void st7735_fill_screen(uint16_t color) {
    st7735_fill_rect(0, 0, ST7735_WIDTH, ST7735_HEIGHT, color);
}

void st7735_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color) {
    st7735_fill_rect(x, y, w, 1, color);
}

void st7735_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;
    if (c < 0x20 || c > 0x7E) c = '?';

    uint16_t cw = 5 * scale;
    uint16_t ch = 7 * scale;
    if (x + cw > ST7735_WIDTH || y + ch > ST7735_HEIGHT) return;

    const uint8_t *glyph = font5x7[c - 0x20];
    uint8_t fg_hi = (uint8_t)(fg >> 8), fg_lo = (uint8_t)(fg & 0xFF);
    uint8_t bg_hi = (uint8_t)(bg >> 8), bg_lo = (uint8_t)(bg & 0xFF);

    /* Fast path for scale == 1 (5x7 = 35 pixels = 70 bytes) */
    if (scale == 1) {
        uint8_t pix[70];
        size_t idx = 0;
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                bool on = (glyph[col] >> row) & 1;
                pix[idx++] = on ? fg_hi : bg_hi;
                pix[idx++] = on ? fg_lo : bg_lo;
            }
        }
        lock_spi();
        set_addr_window(x, y, x + 4, y + 6);
        dc_data();
        cs_select();
        spi_write_blocking(active_spi(), pix, sizeof(pix));
        cs_deselect();
        unlock_spi();
        return;
    }

    /* Scaled path */
    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            uint16_t color = ((line >> row) & 1) ? fg : bg;
            st7735_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void st7735_draw_string(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale) {
    if (scale == 0) scale = 1;
    uint16_t step = 6 * scale;
    while (*s) {
        if (x + 5 * scale > ST7735_WIDTH) break;
        st7735_draw_char(x, y, *s, fg, bg, scale);
        /* Clear 1px spacing column */
        if (x + step <= ST7735_WIDTH) {
            st7735_fill_rect(x + 5 * scale, y, scale, 7 * scale, bg);
        }
        x += step;
        s++;
    }
}
