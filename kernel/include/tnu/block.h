#ifndef TNU_BLOCK_H
#define TNU_BLOCK_H

#include <tnu/types.h>

struct block_device_info {
    const char *name;
    const char *description;
    bool writable;
    uint64_t sector_count;
    uint32_t sector_size;
    bool removable;
    const char *transport;
};

/* Block device functions */
size_t block_device_count(void);
const struct block_device_info *block_device_get(size_t index);
const struct block_device_info *block_device_find(const char *name);
int block_read(const char *name, uint64_t lba, void *data, size_t bytes);
int block_sync(const char *name);
int block_write_lba28(const char *name, uint32_t lba, const void *data, size_t bytes);

/* ATA functions */
bool ata_primary_master_present(void);
int ata_write_lba28(uint32_t lba, const void *data, size_t bytes);

/* NVMe functions */
void nvme_init(void);
int nvme_read_sectors(const char *name, uint64_t lba, void *buf, size_t bytes);
int nvme_write_sectors(const char *name, uint64_t lba, const void *buf, size_t bytes);
int nvme_sync(const char *name);

#endif
