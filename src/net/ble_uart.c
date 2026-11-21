#include "ble_uart.h"
#include "wifi_ap.h"
#include "pico_ble.h"
#include "btstack.h"
#include "ble/gatt-service/nordic_spp_service_server.h"
#include <stdio.h>
#include <string.h>

/*
 * Nordic UART Service (NUS) UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * Little-endian byte order for BLE Scan Response:
 *   9E CA DC 24 0E E5 A9 E0 93 F3 A3 B5 01 00 40 6E
 */
static const uint8_t adv_data[] = {
    /* Flags: LE General Discoverable, BR/EDR Not Supported */
    0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
    /* Complete Local Name: "PicoOS-BLE" (10 chars) */
    0x0B, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
    'P', 'i', 'c', 'o', 'O', 'S', '-', 'B', 'L', 'E',
};

static const uint8_t scan_resp_data[] = {
    /* Complete List of 128-bit Service Class UUIDs: Nordic UART Service */
    0x11, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E,
};

static btstack_packet_callback_registration_t hci_event_cb_reg;
static btstack_context_callback_registration_t tx_send_req;

static hci_con_handle_t ble_con_handle = HCI_CON_HANDLE_INVALID;
static bool ble_ready = false;
static bool ble_notify_enabled = false;
static bool ble_tx_pending = false;
static uint32_t ble_tx_pos = 0;

static void start_advertising(void) {
    uint16_t adv_int_min = 0x0030; /* 30 ms */
    uint16_t adv_int_max = 0x0060; /* 60 ms */
    uint8_t adv_type = 0;          /* ADV_IND */
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));

    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data((uint8_t)sizeof(adv_data), (uint8_t *)adv_data);
    gap_scan_response_set_data((uint8_t)sizeof(scan_resp_data), (uint8_t *)scan_resp_data);
    gap_advertisements_enable(1);
}

static void ble_can_send_now_cb(void *context) {
    (void)context;
    ble_tx_pending = false;
    if (!ble_notify_enabled || ble_con_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    uint16_t mtu = att_server_get_mtu(ble_con_handle);
    size_t max_chunk = (mtu > 3U) ? (size_t)(mtu - 3U) : 20U;
    if (max_chunk > 128U) max_chunk = 128U;

    uint8_t buf[128];
    size_t n = wifi_tx_read_from(&ble_tx_pos, buf, max_chunk);
    if (n > 0) {
        nordic_spp_service_server_send(ble_con_handle, buf, (uint16_t)n);
        if (wifi_tx_available(ble_tx_pos) > 0) {
            ble_tx_pending = true;
            tx_send_req.callback = &ble_can_send_now_cb;
            tx_send_req.context = NULL;
            nordic_spp_service_server_request_can_send_now(&tx_send_req, ble_con_handle);
        }
    }
}

static void nordic_spp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    switch (packet_type) {
        case HCI_EVENT_PACKET:
            if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) break;
            switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
                case GATTSERVICE_SUBEVENT_SPP_SERVICE_CONNECTED:
                    ble_con_handle = gattservice_subevent_spp_service_connected_get_con_handle(packet);
                    ble_notify_enabled = true;
                    ble_tx_pending = false;
                    ble_tx_pos = wifi_tx_recent_start(512U);
                    ble_uart_poll();
                    break;
                case GATTSERVICE_SUBEVENT_SPP_SERVICE_DISCONNECTED:
                    ble_notify_enabled = false;
                    ble_tx_pending = false;
                    ble_con_handle = HCI_CON_HANDLE_INVALID;
                    break;
                default:
                    break;
            }
            break;

        case RFCOMM_DATA_PACKET:
            for (uint16_t i = 0; i < size; i++) {
                os_input_push(packet[i]);
            }
            break;

        default:
            break;
    }
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                ble_ready = true;
                start_advertising();
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            ble_notify_enabled = false;
            ble_tx_pending = false;
            ble_con_handle = HCI_CON_HANDLE_INVALID;
            if (ble_ready) {
                start_advertising();
            }
            break;

        default:
            break;
    }
}

void ble_uart_init(void) {
    l2cap_init();
    sm_init();
    att_server_init(profile_data, NULL, NULL);
    nordic_spp_service_server_init(&nordic_spp_packet_handler);

    hci_event_cb_reg.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_cb_reg);

    hci_power_control(HCI_POWER_ON);
}

void ble_uart_poll(void) {
    if (!ble_notify_enabled || ble_tx_pending || ble_con_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }
    if (wifi_tx_available(ble_tx_pos) > 0) {
        ble_tx_pending = true;
        tx_send_req.callback = &ble_can_send_now_cb;
        tx_send_req.context = NULL;
        nordic_spp_service_server_request_can_send_now(&tx_send_req, ble_con_handle);
    }
}

bool ble_uart_is_connected(void) {
    return ble_notify_enabled && (ble_con_handle != HCI_CON_HANDLE_INVALID);
}

void ble_uart_print_status(void) {
    printf("BLE UART : %s (%s)\r\n",
           ble_ready ? "ACTIVE" : "STARTING",
           ble_uart_is_connected() ? "CONNECTED" : "ADVERTISING");
    printf("BLE Name : %s\r\n", BLE_DEVICE_NAME);
    printf("BLE UUID : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (NUS)\r\n");
}
