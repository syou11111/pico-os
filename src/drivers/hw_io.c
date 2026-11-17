#include "hw_io.h"
#include "st7735.h"
#include "../kernel/kernel.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/clocks.h"

static bool adc_initialized = false;
static bool i2c_initialized[2] = { false, false };
static bool spi_user_initialized[2] = { false, false };

bool hw_is_pin_safe(int pin) {
    if (pin < 0 || pin > 28) return false;
    if (pin >= 23 && pin <= 25) return false; /* Internal Pico W pins */
    if (st7735_is_display_pin((uint)pin)) return false;
    return true;
}

int hw_pin_mode(int pin, int mode) {
    if (!hw_is_pin_safe(pin)) return -1;
    gpio_init((uint)pin);
    gpio_disable_pulls((uint)pin);
    if (mode == HW_OUTPUT) {
        gpio_set_dir((uint)pin, GPIO_OUT);
    } else {
        gpio_set_dir((uint)pin, GPIO_IN);
        if (mode == HW_INPUT_PULLUP) {
            gpio_pull_up((uint)pin);
        } else if (mode == HW_INPUT_PULLDOWN) {
            gpio_pull_down((uint)pin);
        }
    }
    return 0;
}

int hw_digital_write(int pin, int val) {
    if (!hw_is_pin_safe(pin)) return -1;
    gpio_put((uint)pin, val != 0);
    return 0;
}

int hw_digital_read(int pin) {
    if (!hw_is_pin_safe(pin)) return -1;
    return gpio_get((uint)pin) ? 1 : 0;
}

int hw_gpio_toggle(int pin) {
    if (!hw_is_pin_safe(pin)) return -1;
    int next = gpio_get((uint)pin) ? 0 : 1;
    gpio_put((uint)pin, next);
    return next;
}

int hw_pwm_write(int pin, int duty) {
    if (!hw_is_pin_safe(pin)) return -1;
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;

    gpio_set_function((uint)pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num((uint)pin);
    uint chan  = pwm_gpio_to_channel((uint)pin);
    pwm_set_clkdiv(slice, 490.0f);
    pwm_set_wrap(slice, 255);
    pwm_set_chan_level(slice, chan, (uint16_t)duty);
    pwm_set_enabled(slice, true);
    return 0;
}

int hw_no_tone(int pin) {
    if (!hw_is_pin_safe(pin)) return -1;
    uint slice = pwm_gpio_to_slice_num((uint)pin);
    pwm_set_enabled(slice, false);
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, 0);
    return 0;
}

int hw_tone(int pin, int freq_hz, int duration_ms) {
    if (!hw_is_pin_safe(pin)) return -1;
    if (freq_hz <= 0) {
        return hw_no_tone(pin);
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t div = 16;
    uint32_t wrap = (sys_hz / (div * (uint32_t)freq_hz)) - 1U;
    while (wrap > 65535U && div < 255U) {
        div *= 2U;
        if (div > 255U) div = 255U;
        wrap = (sys_hz / (div * (uint32_t)freq_hz)) - 1U;
    }
    if (wrap < 2U) wrap = 2U;
    if (wrap > 65535U) wrap = 65535U;

    gpio_set_function((uint)pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num((uint)pin);
    uint chan  = pwm_gpio_to_channel((uint)pin);
    pwm_set_clkdiv(slice, (float)div);
    pwm_set_wrap(slice, (uint16_t)wrap);
    pwm_set_chan_level(slice, chan, (uint16_t)((wrap + 1U) / 2U));
    pwm_set_enabled(slice, true);

    if (duration_ms > 0) {
        os_sleep_ms((uint32_t)duration_ms);
        hw_no_tone(pin);
    }
    return 0;
}

static void ensure_adc_init(void) {
    if (!adc_initialized) {
        adc_init();
        adc_initialized = true;
    }
}

int hw_adc_read(int pin_or_ch) {
    ensure_adc_init();
    int ch = -1;
    if (pin_or_ch >= 26 && pin_or_ch <= 28) {
        if (!hw_is_pin_safe(pin_or_ch)) return -1;
        adc_gpio_init((uint)pin_or_ch);
        ch = pin_or_ch - 26;
    } else if (pin_or_ch >= 0 && pin_or_ch <= 4) {
        if (pin_or_ch <= 2) {
            int gp = 26 + pin_or_ch;
            if (!hw_is_pin_safe(gp)) return -1;
            adc_gpio_init((uint)gp);
        } else if (pin_or_ch == 4) {
            adc_set_temp_sensor_enabled(true);
        }
        ch = pin_or_ch;
    } else {
        return -1;
    }
    adc_select_input((uint)ch);
    return (int)adc_read();
}

int hw_read_temp_c(void) {
    int raw = hw_adc_read(4);
    if (raw < 0) return 0;
    /* V(mV) = raw * 3300 / 4095; Temp(0.1C) = 270 - (V_mV - 706) * 1000 / 1721 */
    int32_t mv = ((int32_t)raw * 3300) / 4095;
    int32_t t10 = 270 - (((mv - 706) * 1000) / 1721);
    return (int)(t10 / 10);
}

static i2c_inst_t *get_i2c(int bus) {
    if (bus == 0) return i2c0;
    if (bus == 1) return i2c1;
    return NULL;
}

int hw_i2c_init(int bus, int sda, int scl, int freq_hz) {
    i2c_inst_t *inst = get_i2c(bus);
    if (!inst) return -1;
    if (!hw_is_pin_safe(sda) || !hw_is_pin_safe(scl) || sda == scl) return -2;

    bool valid = (bus == 0 && (sda % 4) == 0 && (scl % 4) == 1) ||
                 (bus == 1 && (sda % 4) == 2 && (scl % 4) == 3);
    if (!valid) return -3;

    if (freq_hz <= 0) freq_hz = 100000;
    i2c_init(inst, (uint)freq_hz);
    gpio_set_function((uint)sda, GPIO_FUNC_I2C);
    gpio_set_function((uint)scl, GPIO_FUNC_I2C);
    gpio_pull_up((uint)sda);
    gpio_pull_up((uint)scl);
    i2c_initialized[bus] = true;
    return 0;
}

int hw_i2c_write(int bus, uint8_t addr, const uint8_t *buf, size_t len) {
    i2c_inst_t *inst = get_i2c(bus);
    if (!inst || !i2c_initialized[bus] || !buf) return -1;
    int rc = i2c_write_timeout_us(inst, addr, buf, len, false, 50000);
    return (rc < 0) ? -2 : rc;
}

int hw_i2c_read(int bus, uint8_t addr, uint8_t *buf, size_t len) {
    i2c_inst_t *inst = get_i2c(bus);
    if (!inst || !i2c_initialized[bus] || !buf) return -1;
    int rc = i2c_read_timeout_us(inst, addr, buf, len, false, 50000);
    return (rc < 0) ? -2 : rc;
}

int hw_i2c_write_reg(int bus, uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t pkt[2] = { reg, val };
    return hw_i2c_write(bus, addr, pkt, 2);
}

int hw_i2c_read_reg(int bus, uint8_t addr, uint8_t reg) {
    i2c_inst_t *inst = get_i2c(bus);
    if (!inst || !i2c_initialized[bus]) return -1;
    if (i2c_write_timeout_us(inst, addr, &reg, 1, true, 50000) < 0) return -2;
    uint8_t val = 0;
    if (i2c_read_timeout_us(inst, addr, &val, 1, false, 50000) < 0) return -3;
    return (int)val;
}

int hw_i2c_scan(int bus, uint8_t *found_addrs, int max_found) {
    i2c_inst_t *inst = get_i2c(bus);
    if (!inst || !i2c_initialized[bus]) return -1;
    int count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_timeout_us(inst, addr, &dummy, 1, false, 5000);
        if (rc >= 0) {
            if (found_addrs && count < max_found) {
                found_addrs[count] = addr;
            }
            count++;
        }
    }
    return count;
}

static spi_inst_t *get_user_spi(int bus) {
    if (bus == 0) return spi0;
    if (bus == 1) return spi1;
    return NULL;
}

int hw_spi_open(int bus, int sck, int mosi, int miso, int freq_hz) {
    spi_inst_t *inst = get_user_spi(bus);
    if (!inst) return -1;
    if (bus == (int)st7735_get_pins()->spi_bus) return -2; /* Display uses this SPI bus */
    if (!hw_is_pin_safe(sck) || !hw_is_pin_safe(mosi)) return -3;
    if (miso >= 0 && !hw_is_pin_safe(miso)) return -3;

    bool valid = false;
    if (bus == 0) {
        bool ok_sck  = (sck == 2 || sck == 6 || sck == 18);
        bool ok_mosi = (mosi == 3 || mosi == 7 || mosi == 19);
        bool ok_miso = (miso < 0 || miso == 0 || miso == 4 || miso == 16);
        valid = ok_sck && ok_mosi && ok_miso;
    } else {
        bool ok_sck  = (sck == 10 || sck == 14);
        bool ok_mosi = (mosi == 11 || mosi == 15);
        bool ok_miso = (miso < 0 || miso == 8 || miso == 12);
        valid = ok_sck && ok_mosi && ok_miso;
    }
    if (!valid) return -4;

    if (freq_hz <= 0) freq_hz = 1000000;
    spi_init(inst, (uint)freq_hz);
    gpio_set_function((uint)sck, GPIO_FUNC_SPI);
    gpio_set_function((uint)mosi, GPIO_FUNC_SPI);
    if (miso >= 0) {
        gpio_set_function((uint)miso, GPIO_FUNC_SPI);
    }
    spi_user_initialized[bus] = true;
    return 0;
}

int hw_spi_transfer(int bus, const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_inst_t *inst = get_user_spi(bus);
    if (!inst || !spi_user_initialized[bus] || len == 0) return -1;
    if (tx && rx) {
        return spi_write_read_blocking(inst, tx, rx, len);
    } else if (tx) {
        return spi_write_blocking(inst, tx, len);
    } else if (rx) {
        return spi_read_blocking(inst, 0xFF, rx, len);
    }
    return -1;
}

int hw_spi_byte(int bus, uint8_t val) {
    uint8_t rx = 0;
    if (hw_spi_transfer(bus, &val, &rx, 1) < 0) return -1;
    return (int)rx;
}
