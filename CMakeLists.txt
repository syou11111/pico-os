cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)

# Include pico_sdk_import.cmake from PICO_SDK_PATH
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_os C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_os
    src/main.c
    src/kernel/kernel.c
    src/kernel/context_switch.S
    src/drivers/st7735.c
    src/drivers/hw_io.c
    src/net/wifi_ap.c
    src/net/ble_uart.c
    src/ui/console.c
    src/fs/fs.c
    src/transfer/transfer.c
    src/shell/shell.c
    src/apps/vi.c
    src/apps/cc.c
)

target_include_directories(pico_os PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(pico_os PRIVATE
    PICO_FLASH_BANK_STORAGE_OFFSET=0x1BE000
)

target_link_libraries(pico_os
    pico_stdlib
    pico_bootrom
    pico_flash
    hardware_flash
    hardware_spi
    hardware_pwm
    hardware_adc
    hardware_i2c
    pico_cyw43_arch_lwip_poll
    pico_btstack_ble
    pico_btstack_cyw43
)

pico_btstack_make_gatt_header(pico_os PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/net/pico_ble.gatt")

# Enable USB serial (stdio) for the interactive OS shell, disable UART stdio
pico_enable_stdio_usb(pico_os 1)
pico_enable_stdio_uart(pico_os 0)

# Generate .uf2, .bin, .hex, .dis
pico_add_extra_outputs(pico_os)
