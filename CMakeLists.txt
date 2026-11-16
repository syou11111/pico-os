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

target_link_libraries(pico_os
    pico_stdlib
    pico_bootrom
    pico_flash
    hardware_flash
    hardware_spi
    hardware_pwm
    pico_cyw43_arch_none
)

# Enable USB serial (stdio) for the interactive OS shell, disable UART stdio
pico_enable_stdio_usb(pico_os 1)
pico_enable_stdio_uart(pico_os 0)

# Generate .uf2, .bin, .hex, .dis
pico_add_extra_outputs(pico_os)
