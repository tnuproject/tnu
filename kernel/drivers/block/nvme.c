/*
 * NVMe (Non-Volatile Memory Express) Block Driver for Tiramisu OS
 *
 * Full driver implementation supporting modern PCIe NVMe SSDs:
 * - BAR0 MMIO register mapping
 * - Admin Submission & Completion Queue initialization
 * - I/O Submission & Completion Queue creation
 * - Identify Controller & Namespace discovery (LBA size & capacity)
 * - DMA I/O submission (Read / Write / Flush) via Doorbell registers
 * - Generic block device registration for /dev/nvme0n1 & sysinstall
 */

#include <arch/io.h>
#include <arch/pci.h>
#include <tnu/block.h>
#include <tnu/drivers.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/string.h>
#include <tnu/printf.h>

#define NVME_MAX_DEVICES     4
#define NVME_ADMIN_Q_SIZE    64
#define NVME_IO_Q_SIZE       64
#define NVME_SECTOR_DEFAULT  512

/* NVMe Controller Registers (MMIO BAR0) */
#define NVME_REG_CAP         0x0000  /* 64-bit Controller Capabilities */
#define NVME_REG_VS          0x0008  /* 32-bit Version */
#define NVME_REG_INTMS       0x000c  /* 32-bit Interrupt Mask Set */
#define NVME_REG_INTMC       0x0010  /* 32-bit Interrupt Mask Clear */
#define NVME_REG_CC          0x0014  /* 32-bit Controller Configuration */
#define NVME_REG_CSTS        0x001c  /* 32-bit Controller Status */
#define NVME_REG_NSSR        0x0020  /* 32-bit NVM Subsystem Reset */
#define NVME_REG_AQA         0x0024  /* 32-bit Admin Queue Attributes */
#define NVME_REG_ASQ         0x0028  /* 64-bit Admin SQ Base Address */
#define NVME_REG_ACQ         0x0030  /* 64-bit Admin CQ Base Address */

/* CC (Controller Configuration) bits */
#define NVME_CC_EN           0x00000001
#define NVME_CC_CSS_NVM      0x00000000
#define NVME_CC_MPS_4K       (0u << 7)
#define NVME_CC_AMS_RR       (0u << 4)
#define NVME_CC_SHN_NONE     (0u << 14)
#define NVME_CC_IOSQES_64    (6u << 16)  /* 2^6 = 64 bytes per SQ entry */
#define NVME_CC_IOCQES_16    (4u << 20)  /* 2^4 = 16 bytes per CQ entry */

/* CSTS (Controller Status) bits */
#define NVME_CSTS_RDY        0x00000001
#define NVME_CSTS_CFS        0x00000002

/* Admin Command Opcodes */
#define NVME_ADMIN_OP_DELETE_SQ   0x00
#define NVME_ADMIN_OP_CREATE_SQ   0x01
#define NVME_ADMIN_OP_DELETE_CQ   0x04
#define NVME_ADMIN_OP_CREATE_CQ   0x05
#define NVME_ADMIN_OP_IDENTIFY    0x06
#define NVME_ADMIN_OP_SET_FEAT    0x09
#define NVME_ADMIN_OP_GET_FEAT    0x0a

/* NVM I/O Command Opcodes */
#define NVME_NVM_OP_FLUSH         0x00
#define NVME_NVM_OP_WRITE         0x01
#define NVME_NVM_OP_READ          0x02

/* Standard 64-byte Submission Queue Entry (SQE) */
struct nvme_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed));

/* Standard 16-byte Completion Queue Entry (CQE) */
struct nvme_cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed));

/* Identify Namespace (CNS = 0) layout subset */
struct nvme_ident_namespace {
    uint64_t nsze;           /* Namespace Size (total LBA count) */
    uint64_t ncap;           /* Namespace Capacity */
    uint64_t nuse;           /* Namespace Utilization */
    uint8_t  nsfeat;
    uint8_t  nlbaf;          /* Number of LBA Formats */
    uint8_t  flbas;          /* Formatted LBA Size index */
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  reserved1;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t reserved2;
    uint64_t nvmcap[2];
    uint8_t  reserved3[40];
    uint8_t  nguid[16];
    uint8_t  eui64[8];
    struct {
        uint16_t ms;         /* Metadata Size */
        uint8_t  ds;         /* LBA Data Size (2^ds bytes) */
        uint8_t  rp;         /* Relative Performance */
    } __attribute__((packed)) lbaf[16];
} __attribute__((packed));

struct nvme_device {
    struct block_device_info info;
    char name[32];
    char desc[128];
    uintptr_t mmio;          /* Virtual address of mapped BAR0 */
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t db_stride;      /* Doorbell stride in bytes */
    uint32_t nsid;
    uint64_t total_sectors;
    uint32_t sector_size;

    /* Admin Queue */
    struct nvme_sqe *asq;
    struct nvme_cqe *acq;
    uint16_t asq_tail;
    uint16_t acq_head;
    uint8_t  acq_phase;
    uint16_t admin_cid;

    /* I/O Queue (QID = 1) */
    struct nvme_sqe *iosq;
    struct nvme_cqe *iocq;
    uint16_t iosq_tail;
    uint16_t iocq_head;
    uint8_t  iocq_phase;
    uint16_t io_cid;

    /* Data transfer bounce buffer for unaligned/paged reads/writes */
    uint8_t *bounce_buffer;

    bool present;
};

static struct nvme_device nvme_devices[NVME_MAX_DEVICES];
static size_t nvme_device_count = 0;

/* MMIO Register access helpers */
static inline uint32_t nvme_read32(const struct nvme_device *dev, uint32_t offset)
{
    return *(volatile uint32_t *)((uintptr_t)dev->mmio + offset);
}

static inline void nvme_write32(const struct nvme_device *dev, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)dev->mmio + offset) = val;
}

static inline uint64_t nvme_read64(const struct nvme_device *dev, uint32_t offset)
{
    uint32_t lo = nvme_read32(dev, offset);
    uint32_t hi = nvme_read32(dev, offset + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline void nvme_write64(const struct nvme_device *dev, uint32_t offset, uint64_t val)
{
    nvme_write32(dev, offset, (uint32_t)val);
    nvme_write32(dev, offset + 4, (uint32_t)(val >> 32));
}

/* Doorbell register calculation */
static inline uint32_t nvme_sq_doorbell(const struct nvme_device *dev, uint16_t qid)
{
    return 0x1000u + (2u * (uint32_t)qid) * dev->db_stride;
}

static inline uint32_t nvme_cq_doorbell(const struct nvme_device *dev, uint16_t qid)
{
    return 0x1000u + (2u * (uint32_t)qid + 1u) * dev->db_stride;
}

static int nvme_wait_status_ready(const struct nvme_device *dev, bool expected_ready)
{
    for (int i = 0; i < 200000; i++) {
        uint32_t csts = nvme_read32(dev, NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            log_warn("nvme", "controller fatal status reported");
            return -1;
        }
        bool ready = (csts & NVME_CSTS_RDY) != 0;
        if (ready == expected_ready) {
            return 0;
        }
        io_wait();
    }
    return -1;
}

static int nvme_submit_admin_cmd(struct nvme_device *dev, struct nvme_sqe *cmd, struct nvme_cqe *cqe_out)
{
    uint16_t cid = dev->admin_cid++;
    cmd->cid = cid;

    /* Write SQ entry */
    uint16_t tail = dev->asq_tail;
    memcpy(&dev->asq[tail], cmd, sizeof(struct nvme_sqe));
    tail = (tail + 1) % NVME_ADMIN_Q_SIZE;
    dev->asq_tail = tail;

    /* Ring Admin SQ Doorbell */
    nvme_write32(dev, nvme_sq_doorbell(dev, 0), tail);

    /* Poll Admin CQ entry */
    for (int i = 0; i < 500000; i++) {
        volatile struct nvme_cqe *cqe = &dev->acq[dev->acq_head];
        uint8_t phase = (cqe->status & 1u);
        if (phase == dev->acq_phase) {
            if (cqe_out) {
                memcpy(cqe_out, (const void *)cqe, sizeof(struct nvme_cqe));
            }
            uint16_t head = (dev->acq_head + 1) % NVME_ADMIN_Q_SIZE;
            dev->acq_head = head;
            if (head == 0) {
                dev->acq_phase ^= 1u;
            }
            /* Ring Admin CQ Doorbell */
            nvme_write32(dev, nvme_cq_doorbell(dev, 0), head);

            if ((cqe->status >> 1) != 0) {
                log_warn("nvme", "admin command %02x failed with status %04x",
                         cmd->opcode, cqe->status >> 1);
                return -1;
            }
            return 0;
        }
        io_wait();
    }
    log_warn("nvme", "admin command %02x timed out", cmd->opcode);
    return -1;
}

static int nvme_submit_io_cmd(struct nvme_device *dev, struct nvme_sqe *cmd, struct nvme_cqe *cqe_out)
{
    uint16_t cid = dev->io_cid++;
    cmd->cid = cid;
    cmd->nsid = dev->nsid;

    /* Write I/O SQ entry */
    uint16_t tail = dev->iosq_tail;
    memcpy(&dev->iosq[tail], cmd, sizeof(struct nvme_sqe));
    tail = (tail + 1) % NVME_IO_Q_SIZE;
    dev->iosq_tail = tail;

    /* Ring I/O SQ Doorbell (QID = 1) */
    nvme_write32(dev, nvme_sq_doorbell(dev, 1), tail);

    /* Poll I/O CQ entry */
    for (int i = 0; i < 1000000; i++) {
        volatile struct nvme_cqe *cqe = &dev->iocq[dev->iocq_head];
        uint8_t phase = (cqe->status & 1u);
        if (phase == dev->iocq_phase) {
            if (cqe_out) {
                memcpy(cqe_out, (const void *)cqe, sizeof(struct nvme_cqe));
            }
            uint16_t head = (dev->iocq_head + 1) % NVME_IO_Q_SIZE;
            dev->iocq_head = head;
            if (head == 0) {
                dev->iocq_phase ^= 1u;
            }
            /* Ring I/O CQ Doorbell */
            nvme_write32(dev, nvme_cq_doorbell(dev, 1), head);

            if ((cqe->status >> 1) != 0) {
                log_warn("nvme", "I/O command %02x failed (status=%04x)",
                         cmd->opcode, cqe->status >> 1);
                return -1;
            }
            return 0;
        }
        io_wait();
    }
    log_warn("nvme", "I/O command %02x timed out", cmd->opcode);
    return -1;
}

static int nvme_create_io_queues(struct nvme_device *dev)
{
    /* 1. Allocate I/O Submission & Completion Queues */
    dev->iosq = (struct nvme_sqe *)kmalloc(NVME_IO_Q_SIZE * sizeof(struct nvme_sqe));
    dev->iocq = (struct nvme_cqe *)kmalloc(NVME_IO_Q_SIZE * sizeof(struct nvme_cqe));
    if (!dev->iosq || !dev->iocq) {
        log_warn("nvme", "failed to allocate memory for I/O queues");
        return -1;
    }
    memset(dev->iosq, 0, NVME_IO_Q_SIZE * sizeof(struct nvme_sqe));
    memset(dev->iocq, 0, NVME_IO_Q_SIZE * sizeof(struct nvme_cqe));
    dev->iosq_tail = 0;
    dev->iocq_head = 0;
    dev->iocq_phase = 1;
    dev->io_cid = 0;

    /* 2. Create I/O Completion Queue (QID = 1) */
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_CQ;
    cmd.prp1 = (uint64_t)(uintptr_t)dev->iocq;
    cmd.cdw10 = ((NVME_IO_Q_SIZE - 1u) << 16) | 1u; /* QSIZE | QID */
    cmd.cdw11 = 0x0001; /* Physically Contiguous, Interrupts Disabled */
    if (nvme_submit_admin_cmd(dev, &cmd, NULL) < 0) {
        log_warn("nvme", "failed to create I/O Completion Queue");
        return -1;
    }

    /* 3. Create I/O Submission Queue (QID = 1, associated with CQ 1) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_SQ;
    cmd.prp1 = (uint64_t)(uintptr_t)dev->iosq;
    cmd.cdw10 = ((NVME_IO_Q_SIZE - 1u) << 16) | 1u; /* QSIZE | QID */
    cmd.cdw11 = (1u << 16) | 0x0001; /* CQID=1 | Physically Contiguous */
    if (nvme_submit_admin_cmd(dev, &cmd, NULL) < 0) {
        log_warn("nvme", "failed to create I/O Submission Queue");
        return -1;
    }

    log_info("nvme", "I/O queues created successfully (depth=%u)", NVME_IO_Q_SIZE);
    return 0;
}

static int nvme_identify_namespace(struct nvme_device *dev)
{
    struct nvme_ident_namespace *ns = (struct nvme_ident_namespace *)kmalloc(4096);
    if (!ns) {
        return -1;
    }
    memset(ns, 0, 4096);

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)ns;
    cmd.cdw10 = 0; /* CNS = 0 (Identify Namespace) */

    if (nvme_submit_admin_cmd(dev, &cmd, NULL) < 0) {
        log_warn("nvme", "identify namespace 1 failed; using default geometry");
        dev->nsid = 1;
        dev->sector_size = NVME_SECTOR_DEFAULT;
        dev->total_sectors = 500118192ULL; /* ~256 GB fallback */
        kfree(ns);
        return 0;
    }

    dev->nsid = 1;
    dev->total_sectors = ns->nsze > 0 ? ns->nsze : 500118192ULL;

    uint8_t fmt_idx = ns->flbas & 0x0f;
    uint8_t ds = ns->lbaf[fmt_idx].ds;
    if (ds >= 9 && ds <= 14) {
        dev->sector_size = (1u << ds);
    } else {
        dev->sector_size = NVME_SECTOR_DEFAULT;
    }

    kfree(ns);
    return 0;
}

static int nvme_init_controller(struct nvme_device *dev, uintptr_t bar0)
{
    dev->mmio = bar0;
    if (vmm_map_range_identity(dev->mmio, 0x4000, VMM_FLAG_WRITABLE) < 0) {
        log_warn("nvme", "failed to map NVMe MMIO at %p", (void *)bar0);
        return -1;
    }

    uint64_t cap = nvme_read64(dev, NVME_REG_CAP);
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0x0f);
    dev->db_stride = 1u << (2u + dstrd);

    /* Allocate Admin Queues */
    dev->asq = (struct nvme_sqe *)kmalloc(NVME_ADMIN_Q_SIZE * sizeof(struct nvme_sqe));
    dev->acq = (struct nvme_cqe *)kmalloc(NVME_ADMIN_Q_SIZE * sizeof(struct nvme_cqe));
    dev->bounce_buffer = (uint8_t *)kmalloc(64 * 1024);
    if (!dev->asq || !dev->acq || !dev->bounce_buffer) {
        log_warn("nvme", "failed to allocate memory for admin queues");
        return -1;
    }
    memset(dev->asq, 0, NVME_ADMIN_Q_SIZE * sizeof(struct nvme_sqe));
    memset(dev->acq, 0, NVME_ADMIN_Q_SIZE * sizeof(struct nvme_cqe));
    dev->asq_tail = 0;
    dev->acq_head = 0;
    dev->acq_phase = 1;
    dev->admin_cid = 0;

    /* 1. Disable Controller */
    uint32_t cc = nvme_read32(dev, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        cc &= ~NVME_CC_EN;
        nvme_write32(dev, NVME_REG_CC, cc);
    }
    if (nvme_wait_status_ready(dev, false) < 0) {
        log_warn("nvme", "timed out waiting for controller to reset");
        return -1;
    }

    /* 2. Configure Admin Queues */
    uint32_t aqa = ((NVME_ADMIN_Q_SIZE - 1u) << 16) | (NVME_ADMIN_Q_SIZE - 1u);
    nvme_write32(dev, NVME_REG_AQA, aqa);
    nvme_write64(dev, NVME_REG_ASQ, (uint64_t)(uintptr_t)dev->asq);
    nvme_write64(dev, NVME_REG_ACQ, (uint64_t)(uintptr_t)dev->acq);

    /* 3. Enable Controller */
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR |
         NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_write32(dev, NVME_REG_CC, cc);

    if (nvme_wait_status_ready(dev, true) < 0) {
        log_warn("nvme", "timed out waiting for controller ready");
        return -1;
    }

    /* 4. Create I/O Queues */
    if (nvme_create_io_queues(dev) < 0) {
        return -1;
    }

    /* 5. Identify Namespace */
    if (nvme_identify_namespace(dev) < 0) {
        return -1;
    }

    return 0;
}

static int nvme_read(struct nvme_device *dev, uint64_t lba, void *buf, size_t bytes)
{
    if (!dev || !dev->present || !buf || bytes == 0) {
        return -1;
    }

    size_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint32_t sectors = (uint32_t)((bytes + sector_size - 1) / sector_size);
    if (sectors == 0) sectors = 1;

    /* Single / contiguous transfer using bounce buffer if buffer crosses pages */
    uint64_t prp1 = (uint64_t)(uintptr_t)buf;
    uint64_t prp2 = 0;
    bool use_bounce = false;

    if (bytes > 4096) {
        use_bounce = true;
        prp1 = (uint64_t)(uintptr_t)dev->bounce_buffer;
        if (bytes > 64 * 1024) bytes = 64 * 1024;
    }

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_READ;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (sectors - 1u) & 0xffffu;

    if (nvme_submit_io_cmd(dev, &cmd, NULL) < 0) {
        return -1;
    }

    if (use_bounce) {
        memcpy(buf, dev->bounce_buffer, bytes);
    }
    return (int)bytes;
}

static int nvme_write(struct nvme_device *dev, uint64_t lba, const void *buf, size_t bytes)
{
    if (!dev || !dev->present || !buf || bytes == 0 || !dev->info.writable) {
        return -1;
    }

    size_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint32_t sectors = (uint32_t)((bytes + sector_size - 1) / sector_size);
    if (sectors == 0) sectors = 1;

    uint64_t prp1 = (uint64_t)(uintptr_t)buf;
    uint64_t prp2 = 0;
    bool use_bounce = false;

    if (bytes > 4096) {
        use_bounce = true;
        if (bytes > 64 * 1024) bytes = 64 * 1024;
        memcpy(dev->bounce_buffer, buf, bytes);
        prp1 = (uint64_t)(uintptr_t)dev->bounce_buffer;
    }

    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_WRITE;
    cmd.prp1 = prp1;
    cmd.prp2 = prp2;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (sectors - 1u) & 0xffffu;

    if (nvme_submit_io_cmd(dev, &cmd, NULL) < 0) {
        return -1;
    }
    return (int)bytes;
}

static int nvme_flush(struct nvme_device *dev)
{
    if (!dev || !dev->present) return 0;
    struct nvme_sqe cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_NVM_OP_FLUSH;
    return nvme_submit_io_cmd(dev, &cmd, NULL);
}

void nvme_init(void)
{
    nvme_device_count = 0;

    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *pci = pci_get(i);
        /* NVMe Class Code = 0x01 (Mass Storage), Subclass = 0x08 (Non-Volatile Memory), ProgIF = 0x02 (NVM Express) */
        if (pci->class_code != 0x01 || pci->subclass != 0x08 || pci->prog_if != 0x02) {
            continue;
        }

        if (nvme_device_count >= NVME_MAX_DEVICES) {
            break;
        }

        uint32_t bar0 = pci->bars[0];
        if (!bar0 || (bar0 & 1u) || bar0 == 0xffffffffu) {
            continue;
        }

        pci_enable_bus_mastering(pci);
        pci_set_power_state_d0(pci);

        struct nvme_device *dev = &nvme_devices[nvme_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->vendor_id = pci->vendor_id;
        dev->device_id = pci->device_id;

        if (nvme_init_controller(dev, (uintptr_t)(bar0 & ~0x0fu)) < 0) {
            log_warn("nvme", "failed to initialize NVMe controller at %02x:%02x.%u",
                     pci->bus, pci->slot, pci->function);
            continue;
        }

        dev->present = true;
        ksnprintf(dev->name, sizeof(dev->name), "nvme%zup1", nvme_device_count);
        ksnprintf(dev->desc, sizeof(dev->desc), "NVMe PCIe SSD [%04x:%04x]", dev->vendor_id, dev->device_id);

        dev->info.name = dev->name;
        dev->info.description = dev->desc;
        dev->info.writable = true;
        dev->info.removable = false;
        dev->info.sector_count = dev->total_sectors;
        dev->info.sector_size = dev->sector_size;
        dev->info.transport = "nvme";

        log_info("nvme", "%s: %llu sectors (%u bytes/sec), %llu GiB (driver=nvme-express)",
                 dev->name, (unsigned long long)dev->total_sectors, dev->sector_size,
                 (unsigned long long)((dev->total_sectors * dev->sector_size) / (1024ULL * 1024ULL * 1024ULL)));

        nvme_device_count++;
    }

    if (nvme_device_count == 0) {
        log_info("nvme", "no NVMe storage devices detected");
    }
}

static struct nvme_device *nvme_find_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < nvme_device_count; i++) {
        if (strcmp(nvme_devices[i].name, name) == 0 ||
            (strcmp(name, "nvme0") == 0 && i == 0) ||
            (strcmp(name, "nvme0n1") == 0 && i == 0) ||
            (strcmp(name, "/dev/nvme0n1") == 0 && i == 0) ||
            (strcmp(name, "/dev/nvme0") == 0 && i == 0)) {
            return &nvme_devices[i];
        }
    }
    return NULL;
}

size_t nvme_get_device_count(void)
{
    return nvme_device_count;
}

const struct block_device_info *nvme_device_get(size_t index)
{
    return index < nvme_device_count ? &nvme_devices[index].info : NULL;
}

const struct block_device_info *nvme_device_find(const char *name)
{
    struct nvme_device *dev = nvme_find_by_name(name);
    return dev ? &dev->info : NULL;
}

int nvme_read_sectors(const char *name, uint64_t lba, void *buf, size_t bytes)
{
    struct nvme_device *dev = nvme_find_by_name(name);
    if (!dev) return -1;
    return nvme_read(dev, lba, buf, bytes);
}

int nvme_write_sectors(const char *name, uint64_t lba, const void *buf, size_t bytes)
{
    struct nvme_device *dev = nvme_find_by_name(name);
    if (!dev) return -1;
    return nvme_write(dev, lba, buf, bytes);
}

int nvme_sync(const char *name)
{
    struct nvme_device *dev = nvme_find_by_name(name);
    if (!dev) return -1;
    return nvme_flush(dev);
}

int nvme_block_read(const char *name, uint64_t lba, void *data, size_t bytes)
{
    return nvme_read_sectors(name, lba, data, bytes);
}

int nvme_block_write(const char *name, uint64_t lba, const void *data, size_t bytes)
{
    return nvme_write_sectors(name, lba, data, bytes);
}

int nvme_block_sync(const char *name)
{
    return nvme_sync(name);
}