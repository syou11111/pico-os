#ifndef PICO_OS_BTSTACK_CONFIG_H
#define PICO_OS_BTSTACK_CONFIG_H

/* BTstack features for PicoOS BLE UART (Nordic SPP / NUS) */
#ifndef ENABLE_BLE
#define ENABLE_BLE
#endif
#define ENABLE_LE_PERIPHERAL
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

/* Keep console clean: no BTstack stdout log spam */
#define WANT_HCI_DUMP 0
#define ENABLE_PRINTF_HEXDUMP

/* BTstack buffer & connection limits (minimal RAM footprint) */
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_GATT_CLIENTS 0
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_L2CAP_CHANNELS 4
#define MAX_NR_L2CAP_SERVICES 3
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 4
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4

#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 2
#define MAX_ATT_DB_SIZE 512

/* Flow control */
#define HCI_HOST_ACL_PACKET_LEN (255 + 4)
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 0
#define HCI_HOST_SCO_PACKET_NUM 0

/* Non-volatile storage */
#define NVM_NUM_DEVICE_DB_ENTRIES 4
#define NVM_NUM_LINK_KEYS 4

/* Platform hooks */
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif /* PICO_OS_BTSTACK_CONFIG_H */
