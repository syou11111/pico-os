#ifndef PICO_OS_BLE_UART_H
#define PICO_OS_BLE_UART_H

#include <stdbool.h>
#include <stdint.h>

#define BLE_DEVICE_NAME "PicoOS-BLE"

void ble_uart_init(void);
void ble_uart_poll(void);
bool ble_uart_is_connected(void);
void ble_uart_print_status(void);

#endif /* PICO_OS_BLE_UART_H */
