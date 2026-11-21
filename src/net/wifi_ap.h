#ifndef PICO_OS_WIFI_AP_H
#define PICO_OS_WIFI_AP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WIFI_AP_SSID  "PicoOS-AP"
#define WIFI_AP_PASS  "picoos123"
#define WIFI_AP_IP    "192.168.4.1"

void wifi_ap_init_and_start(void);
void wifi_ap_task(void *arg);
void wifi_ap_print_status(void);
bool wifi_ap_is_ready(void);
void wifi_ap_set_led(bool on);

/* Unified input queue (Wi-Fi keyboard + USB Serial) */
void os_input_push(uint8_t ch);
void os_input_push_str(const char *s);
int  os_getchar_timeout_us(uint32_t timeout_us);

/* Output mirror ring for smartphone / BLE display */
void     wifi_log_output(const char *s, size_t len);
uint32_t wifi_tx_available(uint32_t pos);
uint32_t wifi_tx_recent_start(uint32_t max_back);
size_t   wifi_tx_read_from(uint32_t *pos, uint8_t *out, size_t max_len);

#endif /* PICO_OS_WIFI_AP_H */
