#ifndef TNU_BLOCK_H
#define TNU_BLOCK_H

#include <tnu/types.h>

struct block_device_info {
    const char *name;
    const char *description;
    bool writable;
};

size_t block_device_count(void);
const struct block_device_info *block_device_get(size_t index);
const struct block_device_info *block_device_find(const char *name);
int block_write_lba28(const char *name, uint32_t lba, const void *data, size_t bytes);

bool ata_primary_master_present(void);
int ata_write_lba28(uint32_t lba, const void *data, size_t bytes);

#endif
