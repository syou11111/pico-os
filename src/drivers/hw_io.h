#ifndef PICO_OS_HW_IO_H
#define PICO_OS_HW_IO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HW_INPUT          0
#define HW_OUTPUT         1
#define HW_INPUT_PULLUP   2
#define HW_INPUT_PULLDOWN 3

bool hw_is_pin_safe(int pin);

/* Digital GPIO */
int  hw_pin_mode(int pin, int mode);
int  hw_digital_write(int pin, int val);
int  hw_digital_read(int pin);
int  hw_gpio_toggle(int pin);

/* PWM & Sound */
int  hw_pwm_write(int pin, int duty);
int  hw_tone(int pin, int freq_hz, int duration_ms);
int  hw_no_tone(int pin);

/* ADC & Internal Temperature */
int  hw_adc_read(int pin_or_ch);
int  hw_read_temp_c(void);

/* I2C */
int  hw_i2c_init(int bus, int sda, int scl, int freq_hz);
int  hw_i2c_write(int bus, uint8_t addr, const uint8_t *buf, size_t len);
int  hw_i2c_read(int bus, uint8_t addr, uint8_t *buf, size_t len);
int  hw_i2c_write_reg(int bus, uint8_t addr, uint8_t reg, uint8_t val);
int  hw_i2c_read_reg(int bus, uint8_t addr, uint8_t reg);
int  hw_i2c_scan(int bus, uint8_t *found_addrs, int max_found);

/* SPI */
int  hw_spi_open(int bus, int sck, int mosi, int miso, int freq_hz);
int  hw_spi_transfer(int bus, const uint8_t *tx, uint8_t *rx, size_t len);
int  hw_spi_byte(int bus, uint8_t val);

#endif /* PICO_OS_HW_IO_H */
