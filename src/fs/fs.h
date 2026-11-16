#ifndef PICO_OS_FS_H
#define PICO_OS_FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FS_MAX_FILES     64
#define FS_MAX_FILENAME  44
#define FS_MAX_FILESIZE  (4096 - 64) /* 4032 bytes per file */

typedef struct {
    char     name[FS_MAX_FILENAME];
    uint32_t size;
} fs_file_info_t;

void fs_init(void);
int  fs_write(const char *name, const void *data, size_t len);
int  fs_append(const char *name, const void *data, size_t len);
int  fs_read(const char *name, void *buf, size_t max_len);
int  fs_delete(const char *name);
int  fs_list(fs_file_info_t *out, int max_entries);
void fs_format(void);
void fs_stat(uint32_t *used_files, uint32_t *total_files, uint32_t *used_bytes, uint32_t *total_bytes);

#endif /* PICO_OS_FS_H */
