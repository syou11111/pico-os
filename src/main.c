#include "pico/stdlib.h"
#include "kernel/kernel.h"
#include "drivers/st7735.h"
#include "ui/console.h"
#include "fs/fs.h"
#include "net/wifi_ap.h"
#include "shell/shell.h"

volatile bool g_led_auto_blink = true;

void set_onboard_led(bool on) {
    wifi_ap_set_led(on);
}

static void ui_task(void *arg) {
    (void)arg;
    bool heartbeat = false;
    while (1) {
        heartbeat = !heartbeat;
        console_update_statusbar(os_get_ticks(), os_get_task_count(), heartbeat);
        os_sleep_ms(500);
    }
}

static void led_task(void *arg) {
    (void)arg;
    bool state = false;
    while (1) {
        if (g_led_auto_blink) {
            state = !state;
            set_onboard_led(state);
        }
        os_sleep_ms(500);
    }
}

int main(void) {
    stdio_init_all();
    wifi_ap_init_and_start();

    fs_init();
    st7735_load_saved_pins();
    st7735_init();
    console_init();

    static fs_file_info_t init_list[1];
    if (fs_list(init_list, 1) == 0) {
        shell_seed_samples();
    }

    console_puts("Booting PicoOS v0.6...\n", COLOR_CYAN);
    console_puts("Arch : ARM Cortex-M0+\n", COLOR_WHITE);
    console_puts("Disp : ST7735S 160x128\n", COLOR_WHITE);
    console_puts("FS   : PicoFS 256KB OK\n", COLOR_GREEN);
    console_puts("HW   : GPIO/PWM/ADC/I2C/SPI\n", COLOR_GREEN);
    console_puts("WiFi : PicoOS-AP (4.1)\n", COLOR_CYAN);
    console_puts("Pass : picoos123\n", COLOR_YELLOW);

    os_init();
    os_create_task("ui_bar",    ui_task,      NULL);
    os_create_task("shell",     shell_task,   NULL);
    os_create_task("led_blink", led_task,     NULL);
    os_create_task("wifi_d",    wifi_ap_task, NULL);

    os_start();

    while (1) {
        __asm volatile("wfi");
    }
    return 0;
}
