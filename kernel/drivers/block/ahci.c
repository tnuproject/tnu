/*
 * AHCI SATA driver for Tiramisu OS.
 *
 * This replaces the previous detection-only ahci.c. It exposes AHCI disks to the
 * generic block layer through ahci_get_device_count()/ahci_device_get()/
 * ahci_device_find()/ahci_block_read()/ahci_block_write()/ahci_block_sync().
 *
 * Assumptions matching the rest of this hobby kernel tree:
 *  - PCI BAR5 MMIO is identity-mapped before dereferencing the HBA registers.
 *  - Static DMA buffers are identity-mapped or otherwise DMA-visible.
 *  - Interrupts are not required; commands are completed by polling PxCI/PxIS.
 */

#include <arch/pci.h>
#include <tnu/block.h>
#include <tnu/drivers.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/string.h>
#include <tnu/printf.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AHCI_MAX_CONTROLLERS 4
#define AHCI_MAX_DEVICES     8
#define AHCI_MAX_PORTS       32
#define AHCI_CMD_SLOTS       32
#define AHCI_SECTOR_SIZE     512u
#define AHCI_MAX_PRDT_BYTES  (128u * AHCI_SECTOR_SIZE)
#define AHCI_HBA_MMIO_BYTES   0x2000u

#define AHCI_CLASS_MASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA      0x06
#define AHCI_PROGIF_AHCI        0x01

#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

#define SATA_SIG_ATA   0x00000101u
#define SATA_SIG_ATAPI 0xeb140101u
#define SATA_SIG_SEMB  0xc33c0101u
#define SATA_SIG_PM    0x96690101u

#define HBA_PxCMD_ST   (1u << 0)
#define HBA_PxCMD_FRE  (1u << 4)
#define HBA_PxCMD_FR   (1u << 14)
#define HBA_PxCMD_CR   (1u << 15)
#define HBA_PxCMD_ICC_ACTIVE (1u << 28)

#define HBA_PxIS_TFES  (1u << 30)

#define ATA_DEV_BUSY   0x80
#define ATA_DEV_DRQ    0x08

#define ATA_CMD_IDENTIFY_DMA_EXT 0x25 /* READ DMA EXT works for data-in; IDENTIFY uses 0xec below */
#define ATA_CMD_IDENTIFY         0xec
#define ATA_CMD_READ_DMA_EXT     0x25
#define ATA_CMD_WRITE_DMA_EXT    0x35
#define ATA_CMD_FLUSH_CACHE_EXT  0xea

#define FIS_TYPE_REG_H2D 0x27

struct hba_port {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
};

struct hba_mem {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
    volatile uint8_t  reserved[0xa0 - 0x2c];
    volatile uint8_t  vendor[0x100 - 0xa0];
    struct hba_port ports[AHCI_MAX_PORTS];
};

struct hba_cmd_header {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  reserved0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved1[4];
} __attribute__((packed));

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc:22;
    uint32_t reserved1:9;
    uint32_t i:1;
} __attribute__((packed));

struct hba_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct hba_prdt_entry prdt_entry[8];
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t reserved0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
} __attribute__((packed));

struct ahci_device {
    struct block_device_info info;
    struct hba_mem *abar;
    struct hba_port *port;
    uint8_t port_no;
    bool present;
};

static struct ahci_device ahci_devices[AHCI_MAX_DEVICES];
static size_t ahci_device_count;
static char ahci_names[AHCI_MAX_DEVICES][8];
static char ahci_descs[AHCI_MAX_DEVICES][96];

static struct hba_cmd_header ahci_cmd_lists[AHCI_MAX_DEVICES][AHCI_CMD_SLOTS]
    __attribute__((aligned(1024)));
static uint8_t ahci_fis_buffers[AHCI_MAX_DEVICES][256]
    __attribute__((aligned(256)));
static struct hba_cmd_table ahci_cmd_tables[AHCI_MAX_DEVICES][AHCI_CMD_SLOTS]
    __attribute__((aligned(128)));
static uint8_t ahci_identify_buffers[AHCI_MAX_DEVICES][AHCI_SECTOR_SIZE]
    __attribute__((aligned(4096)));
static uint8_t ahci_bounce[AHCI_MAX_PRDT_BYTES]
    __attribute__((aligned(4096)));

static uint32_t ahci_lo32(const void *p)
{
    return (uint32_t)((uintptr_t)p & 0xffffffffu);
}

static uint32_t ahci_hi32(const void *p)
{
    return (uint32_t)(((uint64_t)(uintptr_t)p) >> 32);
}


static uint64_t ahci_bar5_phys(uint16_t bus, uint8_t slot, uint8_t func, uint32_t bar5_lo)
{
    uint64_t phys = (uint64_t)(bar5_lo & ~0x0full);

    /* AHCI ABAR may legally be a 64-bit memory BAR. If bit 2 is set,
       BAR6 contains the high 32 bits. */
    if ((bar5_lo & 0x6u) == 0x4u) {
        uint32_t bar5_hi = pci_config_read32(bus, slot, func, 0x28);
        phys |= ((uint64_t)bar5_hi << 32);
    }

    return phys;
}

static struct hba_mem *ahci_map_abar(uint16_t bus, uint8_t slot, uint8_t func, uint32_t bar5)
{
    if (bar5 & 0x1u) {
        log_warn("ahci", "controller %02x:%02x.%u BAR5 is I/O space, expected MMIO",
                 bus, slot, func);
        return NULL;
    }

    uint64_t phys64 = ahci_bar5_phys(bus, slot, func, bar5);
    if (!phys64 || phys64 >= 0x0000800000000000ULL) {
        log_warn("ahci", "controller %02x:%02x.%u invalid ABAR=0x%llx",
                 bus, slot, func, (unsigned long long)phys64);
        return NULL;
    }

    uintptr_t phys = (uintptr_t)phys64;
    uintptr_t map_base = phys & ~(uintptr_t)(PAGE_SIZE - 1);
    size_t map_len = (size_t)((phys - map_base) + AHCI_HBA_MMIO_BYTES);

    if (vmm_map_range_identity(map_base, map_len, 0) < 0) {
        log_warn("ahci", "controller %02x:%02x.%u failed to map ABAR=0x%llx",
                 bus, slot, func, (unsigned long long)phys64);
        return NULL;
    }

    return (struct hba_mem *)phys;
}

static void ahci_sleep_poll(void)
{
    for (volatile unsigned i = 0; i < 1000; i++) {
    }
}

static int ahci_wait_clear(volatile uint32_t *reg, uint32_t mask, size_t loops)
{
    while (loops--) {
        if ((*reg & mask) == 0) {
            return 0;
        }
        ahci_sleep_poll();
    }
    return -1;
}

static int ahci_port_type(struct hba_port *port)
{
    uint32_t ssts = port->ssts;
    uint8_t det = (uint8_t)(ssts & 0x0f);
    uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0f);
    if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) {
        return 0;
    }
    switch (port->sig) {
    case SATA_SIG_ATA: return 1;
    case SATA_SIG_ATAPI: return 2;
    case SATA_SIG_SEMB: return 3;
    case SATA_SIG_PM: return 4;
    default: return 0;
    }
}

static void ahci_stop_cmd(struct hba_port *port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    ahci_wait_clear(&port->cmd, HBA_PxCMD_FR | HBA_PxCMD_CR, 1000000);
}

static void ahci_start_cmd(struct hba_port *port)
{
    ahci_wait_clear(&port->cmd, HBA_PxCMD_CR, 1000000);
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static int ahci_find_cmdslot(struct hba_port *port)
{
    uint32_t slots = port->sact | port->ci;
    for (int i = 0; i < AHCI_CMD_SLOTS; i++) {
        if ((slots & (1u << i)) == 0) {
            return i;
        }
    }
    return -1;
}

static int ahci_issue(struct ahci_device *dev, uint8_t command, uint64_t lba,
                      uint16_t sector_count, void *buffer, size_t bytes, bool write)
{
    struct hba_port *port = dev->port;
    int slot = ahci_find_cmdslot(port);
    if (slot < 0) {
        return -1;
    }

    struct hba_cmd_header *hdr = &ahci_cmd_lists[dev - ahci_devices][slot];
    struct hba_cmd_table *tbl = &ahci_cmd_tables[dev - ahci_devices][slot];
    memset(hdr, 0, sizeof(*hdr));
    memset(tbl, 0, sizeof(*tbl));

    hdr->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
    hdr->w = write ? 1 : 0;
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba = ahci_lo32(tbl);
    hdr->ctbau = ahci_hi32(tbl);

    if (bytes) {
        tbl->prdt_entry[0].dba = ahci_lo32(buffer);
        tbl->prdt_entry[0].dbau = ahci_hi32(buffer);
        tbl->prdt_entry[0].dbc = (uint32_t)bytes - 1u;
        tbl->prdt_entry[0].i = 1;
    }

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;
    fis->device = 1u << 6; /* LBA mode */
    fis->lba0 = (uint8_t)(lba & 0xff);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xff);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xff);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xff);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xff);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xff);
    fis->countl = (uint8_t)(sector_count & 0xff);
    fis->counth = (uint8_t)((sector_count >> 8) & 0xff);

    port->is = 0xffffffffu;
    port->serr = 0xffffffffu;

    for (size_t spin = 0; spin < 1000000; spin++) {
        if ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) == 0) {
            break;
        }
        if (spin == 999999) {
            return -1;
        }
        ahci_sleep_poll();
    }

    port->ci = 1u << slot;

    for (;;) {
        if ((port->ci & (1u << slot)) == 0) {
            break;
        }
        if (port->is & HBA_PxIS_TFES) {
            return -1;
        }
        ahci_sleep_poll();
    }

    if (port->is & HBA_PxIS_TFES) {
        return -1;
    }
    return 0;
}

static int ahci_identify_device(struct ahci_device *dev)
{
    size_t index = (size_t)(dev - ahci_devices);
    uint8_t *buf = ahci_identify_buffers[index];
    memset(buf, 0, AHCI_SECTOR_SIZE);

    if (ahci_issue(dev, ATA_CMD_IDENTIFY, 0, 1, buf, AHCI_SECTOR_SIZE, false) < 0) {
        return -1;
    }

    const uint16_t *id = (const uint16_t *)buf;
    uint64_t lba48 = ((uint64_t)id[103] << 48) |
                     ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[100]);
    uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
    uint64_t sectors = lba48 ? lba48 : lba28;
    if (sectors == 0) {
        sectors = 1953525168ull; /* safe fallback: 1 TB-like */
    }

    dev->info.sector_count = sectors;
    dev->info.sector_size = AHCI_SECTOR_SIZE;
    dev->info.removable = false;
    dev->info.writable = true;
    dev->info.transport = "ahci";
    return 0;
}

static int ahci_rw(struct ahci_device *dev, uint64_t lba, void *data, size_t bytes, bool write)
{
    if (!dev || !dev->present || (!data && bytes)) {
        return -1;
    }
    uint8_t *ptr = (uint8_t *)data;
    size_t done = 0;

    while (done < bytes) {
        size_t chunk = bytes - done;
        if (chunk > AHCI_MAX_PRDT_BYTES) {
            chunk = AHCI_MAX_PRDT_BYTES;
        }
        uint16_t sectors = (uint16_t)((chunk + AHCI_SECTOR_SIZE - 1u) / AHCI_SECTOR_SIZE);
        size_t dma_bytes = (size_t)sectors * AHCI_SECTOR_SIZE;
        void *dma = ptr + done;

        if (write && (chunk != dma_bytes || (((uintptr_t)dma) & 1u))) {
            memset(ahci_bounce, 0, dma_bytes);
            memcpy(ahci_bounce, ptr + done, chunk);
            dma = ahci_bounce;
        } else if (!write && chunk != dma_bytes) {
            dma = ahci_bounce;
        }

        uint8_t cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        if (ahci_issue(dev, cmd, lba, sectors, dma, dma_bytes, write) < 0) {
            return -1;
        }
        if (!write && dma == ahci_bounce) {
            memcpy(ptr + done, ahci_bounce, chunk);
        }
        lba += sectors;
        done += chunk;
    }
    return 0;
}

static int ahci_flush(struct ahci_device *dev)
{
    if (!dev || !dev->present) {
        return -1;
    }
    return ahci_issue(dev, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, NULL, 0, false);
}

static int ahci_register_port(struct hba_mem *abar, uint8_t port_no)
{
    if (ahci_device_count >= AHCI_MAX_DEVICES) {
        return -1;
    }

    struct hba_port *port = &abar->ports[port_no];
    if (ahci_port_type(port) != 1) {
        return -1;
    }

    size_t index = ahci_device_count;
    struct ahci_device *dev = &ahci_devices[index];
    memset(dev, 0, sizeof(*dev));
    dev->abar = abar;
    dev->port = port;
    dev->port_no = port_no;
    dev->present = true;

    ahci_stop_cmd(port);
    memset(ahci_cmd_lists[index], 0, sizeof(ahci_cmd_lists[index]));
    memset(ahci_fis_buffers[index], 0, sizeof(ahci_fis_buffers[index]));
    memset(ahci_cmd_tables[index], 0, sizeof(ahci_cmd_tables[index]));

    port->clb = ahci_lo32(ahci_cmd_lists[index]);
    port->clbu = ahci_hi32(ahci_cmd_lists[index]);
    port->fb = ahci_lo32(ahci_fis_buffers[index]);
    port->fbu = ahci_hi32(ahci_fis_buffers[index]);
    port->serr = 0xffffffffu;
    port->is = 0xffffffffu;
    port->ie = 0;
    port->cmd |= HBA_PxCMD_ICC_ACTIVE;
    ahci_start_cmd(port);

    if (ahci_identify_device(dev) < 0) {
        log_warn("ahci", "port %u present but IDENTIFY failed", port_no);
        dev->present = false;
        return -1;
    }

    /* Use sdX names so the existing devfs/sysinstall path keeps working. */
    ahci_names[index][0] = 's';
    ahci_names[index][1] = 'd';
    ahci_names[index][2] = (char)('a' + index);
    ahci_names[index][3] = '\0';
    ksnprintf(ahci_descs[index], sizeof(ahci_descs[index]),
              "AHCI SATA disk on port %u", port_no);
    dev->info.name = ahci_names[index];
    dev->info.description = ahci_descs[index];

    log_info("ahci", "%s: port=%u sectors=%llu", dev->info.name, port_no,
             (unsigned long long)dev->info.sector_count);
    ahci_device_count++;
    return 0;
}

void ahci_init(void)
{
    ahci_device_count = 0;
    memset(ahci_devices, 0, sizeof(ahci_devices));

    size_t controllers = 0;
    for (uint16_t bus = 0; bus < 256 && controllers < AHCI_MAX_CONTROLLERS; bus++) {
        for (uint8_t slot = 0; slot < 32 && controllers < AHCI_MAX_CONTROLLERS; slot++) {
            for (uint8_t func = 0; func < 8 && controllers < AHCI_MAX_CONTROLLERS; func++) {
                uint16_t vendor = pci_read_config16(bus, slot, func, 0x00);
                if (vendor == 0xffff) {
                    continue;
                }
                uint8_t class_code = pci_read_config8(bus, slot, func, 0x0b);
                uint8_t subclass = pci_read_config8(bus, slot, func, 0x0a);
                uint8_t progif = pci_read_config8(bus, slot, func, 0x09);
                if (class_code != AHCI_CLASS_MASS_STORAGE || subclass != AHCI_SUBCLASS_SATA ||
                    progif != AHCI_PROGIF_AHCI) {
                    continue;
                }

                uint32_t bar5 = pci_config_read32(bus, slot, func, 0x24);
                if (bar5 == 0 || bar5 == 0xffffffffu) {
                    log_warn("ahci", "controller %02x:%02x.%u has invalid BAR5", bus, slot, func);
                    continue;
                }

                uint16_t cmd = pci_read_config16(bus, slot, func, 0x04);
                cmd |= 0x0002u | 0x0004u; /* memory space + bus master */
                pci_write_config16(bus, slot, func, 0x04, cmd);

                struct hba_mem *abar = ahci_map_abar(bus, slot, func, bar5);
                if (!abar) {
                    continue;
                }

                /* Enable AHCI mode only after the MMIO region is mapped. */
                abar->ghc |= (1u << 31);
                uint32_t pi = abar->pi;
                controllers++;

                log_info("ahci", "controller at %02x:%02x.%u ABAR=0x%llx PI=0x%x",
                         bus, slot, func,
                         (unsigned long long)ahci_bar5_phys(bus, slot, func, bar5), pi);

                for (uint8_t port = 0; port < AHCI_MAX_PORTS; port++) {
                    if (pi & (1u << port)) {
                        ahci_register_port(abar, port);
                    }
                }
            }
        }
    }

    if (!controllers) {
        log_info("ahci", "no AHCI controller detected");
    }
    if (!ahci_device_count) {
        log_info("ahci", "no AHCI SATA disks detected");
    }
}

size_t ahci_get_device_count(void)
{
    return ahci_device_count;
}

const struct block_device_info *ahci_device_get(size_t index)
{
    if (index >= ahci_device_count) {
        return NULL;
    }
    return &ahci_devices[index].info;
}

static struct ahci_device *ahci_by_name(const char *name)
{
    if (!name) {
        return NULL;
    }
    if (strncmp(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (size_t i = 0; i < ahci_device_count; i++) {
        if (strcmp(ahci_devices[i].info.name, name) == 0) {
            return &ahci_devices[i];
        }
    }
    return NULL;
}

const struct block_device_info *ahci_device_find(const char *name)
{
    struct ahci_device *dev = ahci_by_name(name);
    return dev ? &dev->info : NULL;
}

int ahci_block_read(const char *name, uint64_t lba, void *data, size_t bytes)
{
    struct ahci_device *dev = ahci_by_name(name);
    return ahci_rw(dev, lba, data, bytes, false);
}

int ahci_block_write(const char *name, uint64_t lba, const void *data, size_t bytes)
{
    struct ahci_device *dev = ahci_by_name(name);
    return ahci_rw(dev, lba, (void *)data, bytes, true);
}

int ahci_block_sync(const char *name)
{
    return ahci_flush(ahci_by_name(name));
}
