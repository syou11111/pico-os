#include "fs.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include <string.h>

/* Reserve last 256 KB of 2MB Flash for PicoFS */
#define FS_TOTAL_SIZE   (FS_MAX_FILES * FLASH_SECTOR_SIZE)
#define FS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FS_TOTAL_SIZE)
#define FS_MAGIC        0x5049434FUL /* "PICO" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t size;
    char     name[FS_MAX_FILENAME];
    uint8_t  reserved[64 - 8 - FS_MAX_FILENAME];
} fs_header_t;

_Static_assert(sizeof(fs_header_t) == 64, "fs_header_t must be 64 bytes");

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t prog_len;
} flash_write_op_t;

typedef struct {
    uint32_t offset;
    size_t erase_len;
} flash_erase_op_t;

static uint8_t sector_buf[FLASH_SECTOR_SIZE];

static void __no_inline_not_in_flash_func(do_flash_erase)(void *param) {
    flash_erase_op_t *op = (flash_erase_op_t *)param;
    flash_range_erase(op->offset, op->erase_len);
}

static void __no_inline_not_in_flash_func(do_flash_write)(void *param) {
    flash_write_op_t *op = (flash_write_op_t *)param;
    flash_range_erase(op->offset, FLASH_SECTOR_SIZE);
    flash_range_program(op->offset, op->data, op->prog_len);
}

static const fs_header_t *get_slot_header(int slot) {
    uint32_t addr = XIP_BASE + FS_FLASH_OFFSET + (uint32_t)slot * FLASH_SECTOR_SIZE;
    return (const fs_header_t *)addr;
}

static const uint8_t *get_slot_data(int slot) {
    uint32_t addr = XIP_BASE + FS_FLASH_OFFSET + (uint32_t)slot * FLASH_SECTOR_SIZE + sizeof(fs_header_t);
    return (const uint8_t *)addr;
}

static bool is_slot_valid(int slot) {
    const fs_header_t *hdr = get_slot_header(slot);
    return (hdr->magic == FS_MAGIC && hdr->size <= FS_MAX_FILESIZE && hdr->name[0] != '\0');
}

static int find_slot_by_name(const char *name) {
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (is_slot_valid(i) && strncmp(get_slot_header(i)->name, name, FS_MAX_FILENAME) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!is_slot_valid(i)) {
            return i;
        }
    }
    return -1;
}

void fs_init(void) {
    /* Nothing required at boot; XIP memory-mapped access is always ready */
}

int fs_write(const char *name, const void *data, size_t len) {
    if (!name || name[0] == '\0' || strlen(name) >= FS_MAX_FILENAME) {
        return -1;
    }
    if (len > FS_MAX_FILESIZE) {
        return -2;
    }

    int slot = find_slot_by_name(name);
    if (slot < 0) {
        slot = find_free_slot();
    }
    if (slot < 0) {
        return -3; /* Disk full */
    }

    memset(sector_buf, 0xFF, sizeof(sector_buf));
    fs_header_t *hdr = (fs_header_t *)sector_buf;
    memset(hdr, 0, sizeof(fs_header_t));
    hdr->magic = FS_MAGIC;
    hdr->size  = (uint32_t)len;
    strncpy(hdr->name, name, FS_MAX_FILENAME - 1);

    if (len > 0 && data != NULL) {
        memcpy(sector_buf + sizeof(fs_header_t), data, len);
    }

    size_t total = sizeof(fs_header_t) + len;
    size_t prog_len = ((total + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

    flash_write_op_t op = {
        .offset = FS_FLASH_OFFSET + (uint32_t)slot * FLASH_SECTOR_SIZE,
        .data = sector_buf,
        .prog_len = prog_len
    };

    int rc = flash_safe_execute(do_flash_write, &op, UINT32_MAX);
    return (rc == PICO_OK) ? (int)len : -4;
}

int fs_append(const char *name, const void *data, size_t len) {
    if (!name || name[0] == '\0' || strlen(name) >= FS_MAX_FILENAME) {
        return -1;
    }

    int slot = find_slot_by_name(name);
    if (slot < 0) {
        return fs_write(name, data, len);
    }

    const fs_header_t *hdr = get_slot_header(slot);
    uint32_t old_len = hdr->size;
    if (old_len + len > FS_MAX_FILESIZE) {
        return -2;
    }

    static uint8_t tmp[FS_MAX_FILESIZE];
    if (old_len > 0) {
        memcpy(tmp, get_slot_data(slot), old_len);
    }
    if (len > 0 && data != NULL) {
        memcpy(tmp + old_len, data, len);
    }

    return fs_write(name, tmp, old_len + len);
}

int fs_read(const char *name, void *buf, size_t max_len) {
    int slot = find_slot_by_name(name);
    if (slot < 0) return -1;

    const fs_header_t *hdr = get_slot_header(slot);
    size_t copy_len = hdr->size;
    if (copy_len > max_len) {
        copy_len = max_len;
    }
    if (copy_len > 0 && buf != NULL) {
        memcpy(buf, get_slot_data(slot), copy_len);
    }
    return (int)hdr->size;
}

int fs_delete(const char *name) {
    int slot = find_slot_by_name(name);
    if (slot < 0) return -1;

    flash_erase_op_t op = {
        .offset = FS_FLASH_OFFSET + (uint32_t)slot * FLASH_SECTOR_SIZE,
        .erase_len = FLASH_SECTOR_SIZE
    };
    int rc = flash_safe_execute(do_flash_erase, &op, UINT32_MAX);
    return (rc == PICO_OK) ? 0 : -2;
}

int fs_list(fs_file_info_t *out, int max_entries) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES && count < max_entries; i++) {
        if (is_slot_valid(i)) {
            const fs_header_t *hdr = get_slot_header(i);
            strncpy(out[count].name, hdr->name, FS_MAX_FILENAME - 1);
            out[count].name[FS_MAX_FILENAME - 1] = '\0';
            out[count].size = hdr->size;
            count++;
        }
    }
    return count;
}

void fs_format(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (is_slot_valid(i)) {
            flash_erase_op_t op = {
                .offset = FS_FLASH_OFFSET + (uint32_t)i * FLASH_SECTOR_SIZE,
                .erase_len = FLASH_SECTOR_SIZE
            };
            flash_safe_execute(do_flash_erase, &op, UINT32_MAX);
        }
    }
}

void fs_stat(uint32_t *used_files, uint32_t *total_files, uint32_t *used_bytes, uint32_t *total_bytes) {
    uint32_t files = 0;
    uint32_t bytes = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (is_slot_valid(i)) {
            files++;
            bytes += get_slot_header(i)->size;
        }
    }
    if (used_files)  *used_files  = files;
    if (total_files) *total_files = FS_MAX_FILES;
    if (used_bytes)  *used_bytes  = bytes;
    if (total_bytes) *total_bytes = FS_MAX_FILES * FS_MAX_FILESIZE;
}
