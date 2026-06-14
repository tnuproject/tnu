#include <arch/io.h>
#include <tnu/block.h>
#include <tnu/drivers.h>
#include <tnu/log.h>
#include <tnu/string.h>

#define ATA_MAX_DEVICES 4

#define ATA_REG_DATA 0
#define ATA_REG_ERROR 1
#define ATA_REG_FEATURES 1
#define ATA_REG_SECCOUNT 2
#define ATA_REG_LBA0 3
#define ATA_REG_LBA1 4
#define ATA_REG_LBA2 5
#define ATA_REG_HDDEVSEL 6
#define ATA_REG_COMMAND 7
#define ATA_REG_STATUS 7

#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF 0x20
#define ATA_SR_BSY 0x80

#define ATA_CMD_IDENTIFY 0xec
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH 0xe7

struct ata_probe_slot {
    uint16_t io;
    uint16_t ctrl;
    uint8_t drive;
    const char *description;
};

struct ata_device {
    struct block_device_info info;
    uint16_t io;
    uint16_t ctrl;
    uint8_t drive;
    bool present;
};

static const char *device_names[ATA_MAX_DEVICES] = { "sda", "sdb", "sdc", "sdd" };
static const struct ata_probe_slot probe_slots[ATA_MAX_DEVICES] = {
    { 0x1f0, 0x3f6, 0, "legacy ATA primary master" },
    { 0x1f0, 0x3f6, 1, "legacy ATA primary slave" },
    { 0x170, 0x376, 0, "legacy ATA secondary master" },
    { 0x170, 0x376, 1, "legacy ATA secondary slave" },
};

static struct ata_device devices[ATA_MAX_DEVICES];
static size_t device_count;

static void ata_delay(const struct ata_device *dev)
{
    for (int i = 0; i < 4; i++) {
        (void)inb(dev->ctrl);
    }
}

static int ata_wait_not_busy(const struct ata_device *dev)
{
    for (size_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(dev->io + ATA_REG_STATUS);
        if (!(status & ATA_SR_BSY)) {
            return status;
        }
        io_wait();
    }
    return -1;
}

static int ata_wait_drq(const struct ata_device *dev)
{
    for (size_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(dev->io + ATA_REG_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int ata_select(const struct ata_device *dev, uint32_t lba)
{
    uint8_t drive = (uint8_t)(0xe0 | (dev->drive << 4) | ((lba >> 24) & 0x0f));
    outb(dev->io + ATA_REG_HDDEVSEL, drive);
    ata_delay(dev);
    return ata_wait_not_busy(dev) >= 0 ? 0 : -1;
}

static int ata_select_identify(const struct ata_device *dev)
{
    outb(dev->io + ATA_REG_HDDEVSEL, (uint8_t)(0xa0 | (dev->drive << 4)));
    ata_delay(dev);
    return ata_wait_not_busy(dev) >= 0 ? 0 : -1;
}

static bool ata_identify(struct ata_device *dev)
{
    uint16_t identify[256];
    if (ata_select_identify(dev) < 0) {
        return false;
    }

    uint8_t status = inb(dev->io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xff) {
        return false;
    }

    outb(dev->io + ATA_REG_SECCOUNT, 0);
    outb(dev->io + ATA_REG_LBA0, 0);
    outb(dev->io + ATA_REG_LBA1, 0);
    outb(dev->io + ATA_REG_LBA2, 0);
    outb(dev->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    status = inb(dev->io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xff) {
        return false;
    }
    if (ata_wait_not_busy(dev) < 0) {
        return false;
    }

    uint8_t lba1 = inb(dev->io + ATA_REG_LBA1);
    uint8_t lba2 = inb(dev->io + ATA_REG_LBA2);
    if (lba1 != 0 || lba2 != 0) {
        return false;
    }
    if (ata_wait_drq(dev) < 0) {
        return false;
    }

    for (size_t i = 0; i < 256; i++) {
        identify[i] = inw(dev->io + ATA_REG_DATA);
    }
    uint32_t lba28 = ((uint32_t)identify[61] << 16) | identify[60];
    dev->info.sector_count = lba28;
    dev->info.sector_size = 512;
    dev->info.removable = false;
    dev->info.transport = "ata";
    return true;
}

static int ata_flush(const struct ata_device *dev)
{
    if (ata_select(dev, 0) < 0) {
        return -1;
    }
    outb(dev->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy(dev) >= 0 ? 0 : -1;
}

static int ata_write_one_sector(const struct ata_device *dev, uint32_t lba,
                                const uint8_t *src, size_t available)
{
    uint8_t scratch[512];
    if (!dev || !dev->present || lba >= 0x10000000u) {
        return -1;
    }
    if (available < sizeof(scratch)) {
        memset(scratch, 0, sizeof(scratch));
        if (available) {
            memcpy(scratch, src, available);
        }
        src = scratch;
    }

    if (ata_select(dev, lba) < 0) {
        return -1;
    }

    outb(dev->io + ATA_REG_FEATURES, 0);
    outb(dev->io + ATA_REG_SECCOUNT, 1);
    outb(dev->io + ATA_REG_LBA0, (uint8_t)(lba & 0xff));
    outb(dev->io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xff));
    outb(dev->io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xff));
    outb(dev->io + ATA_REG_HDDEVSEL,
         (uint8_t)(0xe0 | (dev->drive << 4) | ((lba >> 24) & 0x0f)));
    outb(dev->io + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);

    if (ata_wait_drq(dev) < 0) {
        return -1;
    }

    for (size_t i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)src[i * 2] | ((uint16_t)src[i * 2 + 1] << 8);
        outw(dev->io + ATA_REG_DATA, word);
    }
    return ata_wait_not_busy(dev) >= 0 ? 0 : -1;
}

static int ata_read_one_sector(const struct ata_device *dev, uint32_t lba, uint8_t *dst)
{
    if (!dev || !dev->present || !dst || lba >= 0x10000000u) {
        return -1;
    }
    if (ata_select(dev, lba) < 0) {
        return -1;
    }

    outb(dev->io + ATA_REG_FEATURES, 0);
    outb(dev->io + ATA_REG_SECCOUNT, 1);
    outb(dev->io + ATA_REG_LBA0, (uint8_t)(lba & 0xff));
    outb(dev->io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xff));
    outb(dev->io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xff));
    outb(dev->io + ATA_REG_HDDEVSEL,
         (uint8_t)(0xe0 | (dev->drive << 4) | ((lba >> 24) & 0x0f)));
    outb(dev->io + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (ata_wait_drq(dev) < 0) {
        return -1;
    }

    for (size_t i = 0; i < 256; i++) {
        uint16_t word = inw(dev->io + ATA_REG_DATA);
        dst[i * 2] = (uint8_t)(word & 0xff);
        dst[i * 2 + 1] = (uint8_t)(word >> 8);
    }
    return 0;
}

static struct ata_device *ata_by_name(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (size_t i = 0; i < device_count; i++) {
        if (strcmp(devices[i].info.name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

/* External AHCI device count - defined in ahci.c */
extern size_t ahci_get_device_count(void);
extern const struct block_device_info *ahci_device_get(size_t index);
extern const struct block_device_info *ahci_device_find(const char *name);
extern int ahci_block_read(const char *name, uint64_t lba, void *data, size_t bytes);
extern int ahci_block_write(const char *name, uint64_t lba, const void *data, size_t bytes);
extern int ahci_block_sync(const char *name);

/* External NVMe device count - defined in nvme.c */
extern size_t nvme_get_device_count(void);
extern const struct block_device_info *nvme_device_get(size_t index);
extern const struct block_device_info *nvme_device_find(const char *name);
extern int nvme_block_read(const char *name, uint64_t lba, void *data, size_t bytes);
extern int nvme_block_write(const char *name, uint64_t lba, const void *data, size_t bytes);
extern int nvme_block_sync(const char *name);

void ata_init(void)
{
    device_count = 0;
    memset(devices, 0, sizeof(devices));

    for (size_t i = 0; i < ATA_MAX_DEVICES; i++) {
        struct ata_device candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.io = probe_slots[i].io;
        candidate.ctrl = probe_slots[i].ctrl;
        candidate.drive = probe_slots[i].drive;

        int status = inb(candidate.io + ATA_REG_STATUS);
        if (status == 0x00 || status == 0xff || !ata_identify(&candidate)) {
            continue;
        }

        if (device_count >= ATA_MAX_DEVICES) {
            break;
        }
        candidate.present = true;
        candidate.info.name = device_names[device_count];
        candidate.info.description = probe_slots[i].description;
        candidate.info.writable = true;
        devices[device_count++] = candidate;
        log_info("ata", "%s detected at io=0x%x drive=%u",
                 candidate.info.name, candidate.io, candidate.drive);
    }

    if (device_count == 0) {
        log_info("ata", "no legacy ATA disks detected");
    }
}

size_t block_device_count(void)
{
    return device_count + ahci_get_device_count() + nvme_get_device_count();
}

const struct block_device_info *block_device_get(size_t index)
{
    if (index < device_count) {
        return &devices[index].info;
    }

    index -= device_count;
    if (index < ahci_get_device_count()) {
        return ahci_device_get(index);
    }

    index -= ahci_get_device_count();
    if (index < nvme_get_device_count()) {
        return nvme_device_get(index);
    }
    return NULL;
}

const struct block_device_info *block_device_find(const char *name)
{
    struct ata_device *dev = ata_by_name(name);
    if (dev) {
        return &dev->info;
    }

    const struct block_device_info *ahci = ahci_device_find(name);
    if (ahci) {
        return ahci;
    }

    return nvme_device_find(name);
}

int block_write_lba28(const char *name, uint32_t lba, const void *data, size_t bytes)
{
    struct ata_device *dev = ata_by_name(name);
    if (dev) {
        if (!data && bytes) {
            return -1;
        }
        const uint8_t *src = data;
        size_t offset = 0;
        while (offset < bytes) {
            size_t remaining = bytes - offset;
            size_t chunk = remaining < 512 ? remaining : 512;
            if (ata_write_one_sector(dev, lba++, src + offset, chunk) < 0) {
                return -1;
            }
            offset += chunk;
        }
        return ata_flush(dev);
    }

    if (ahci_device_find(name)) {
        return ahci_block_write(name, lba, data, bytes);
    }

    return nvme_block_write(name, lba, data, bytes);
}

int block_read(const char *name, uint64_t lba, void *data, size_t bytes)
{
    struct ata_device *dev = ata_by_name(name);
    if (dev) {
        if (!data && bytes) {
            return -1;
        }
        if (lba >= 0x10000000ull) {
            return -1;
        }
        uint8_t sector[512];
        uint8_t *out = data;
        size_t offset = 0;
        while (offset < bytes) {
            if (ata_read_one_sector(dev, (uint32_t)lba++, sector) < 0) {
                return -1;
            }
            size_t chunk = bytes - offset;
            if (chunk > sizeof(sector)) {
                chunk = sizeof(sector);
            }
            memcpy(out + offset, sector, chunk);
            offset += chunk;
        }
        return 0;
    }

    if (ahci_device_find(name)) {
        return ahci_block_read(name, lba, data, bytes);
    }

    return nvme_block_read(name, lba, data, bytes);
}

int block_sync(const char *name)
{
    struct ata_device *dev = ata_by_name(name);
    if (dev) {
        return ata_flush(dev);
    }

    if (ahci_device_find(name)) {
        return ahci_block_sync(name);
    }

    return nvme_block_sync(name);
}

bool ata_primary_master_present(void)
{
    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].io == 0x1f0 && devices[i].drive == 0) {
            return true;
        }
    }
    return false;
}

int ata_write_lba28(uint32_t lba, const void *data, size_t bytes)
{
    if (device_count == 0) {
        return -1;
    }
    return block_write_lba28(devices[0].info.name, lba, data, bytes);
}
