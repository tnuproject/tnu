#include <arch/pci.h>
#include <tnu/linux_driver_runtime.h>
#include <tnu/log.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/vfs.h>
#include <tnu/drm_kms.h>

#define LDR_MAX_WORK 32
#define LDR_MAX_IRQ 32
#define LDR_ERR_UNSUPPORTED (-95)
#define LDR_ERR_BUSY        (-16)
#define LDR_ERR_NOTFOUND    (-2)
#define LDR_ERR_INVALID     (-22)

struct ldr_irq_slot {
    bool used;
    uint8_t irq;
    ldr_irq_handler_t handler;
    void *ctx;
    const char *name;
};

static struct ldr_work_item ldr_work_queue[LDR_MAX_WORK];
static size_t ldr_work_count;
static struct ldr_irq_slot ldr_irq_slots[LDR_MAX_IRQ];

int ldr_queue_work(const char *name, ldr_work_fn_t fn, void *ctx)
{
    if (!fn) {
        return LDR_ERR_INVALID;
    }
    if (ldr_work_count >= LDR_MAX_WORK) {
        return LDR_ERR_BUSY;
    }
    ldr_work_queue[ldr_work_count].name = name ? name : "work";
    ldr_work_queue[ldr_work_count].fn = fn;
    ldr_work_queue[ldr_work_count].ctx = ctx;
    ldr_work_count++;
    ldr_note_work_item();
    return 0;
}

void ldr_run_workqueues(void)
{
    while (ldr_work_count) {
        struct ldr_work_item item = ldr_work_queue[0];
        for (size_t i = 1; i < ldr_work_count; i++) {
            ldr_work_queue[i - 1] = ldr_work_queue[i];
        }
        ldr_work_count--;
        if (item.fn) {
            item.fn(item.ctx);
        }
    }
}

int ldr_request_irq(uint8_t irq, ldr_irq_handler_t handler, void *ctx, const char *name)
{
    if (!handler) {
        return LDR_ERR_INVALID;
    }
    for (size_t i = 0; i < LDR_MAX_IRQ; i++) {
        if (ldr_irq_slots[i].used && ldr_irq_slots[i].irq == irq) {
            return LDR_ERR_BUSY;
        }
    }
    for (size_t i = 0; i < LDR_MAX_IRQ; i++) {
        if (!ldr_irq_slots[i].used) {
            ldr_irq_slots[i].used = true;
            ldr_irq_slots[i].irq = irq;
            ldr_irq_slots[i].handler = handler;
            ldr_irq_slots[i].ctx = ctx;
            ldr_irq_slots[i].name = name ? name : "linuxdrv";
            ldr_note_irq_redirect();
            return 0;
        }
    }
    return LDR_ERR_BUSY;
}

int ldr_free_irq(uint8_t irq, void *ctx)
{
    for (size_t i = 0; i < LDR_MAX_IRQ; i++) {
        if (ldr_irq_slots[i].used && ldr_irq_slots[i].irq == irq &&
            (!ctx || ldr_irq_slots[i].ctx == ctx)) {
            memset(&ldr_irq_slots[i], 0, sizeof(ldr_irq_slots[i]));
            return 0;
        }
    }
    return LDR_ERR_NOTFOUND;
}

int ldr_dispatch_irq(uint8_t irq)
{
    for (size_t i = 0; i < LDR_MAX_IRQ; i++) {
        if (ldr_irq_slots[i].used && ldr_irq_slots[i].irq == irq) {
            ldr_irq_slots[i].handler(ldr_irq_slots[i].ctx);
            return 0;
        }
    }
    return LDR_ERR_NOTFOUND;
}

int ldr_dma_map(void *cpu_addr, size_t size, bool streaming, struct ldr_dma_mapping *out)
{
    if (!cpu_addr || !size || !out) {
        return LDR_ERR_INVALID;
    }
    out->cpu_addr = cpu_addr;
    out->dma_addr = (uintptr_t)cpu_addr;
    out->size = size;
    out->streaming = streaming;
    ldr_note_dma_map();
    return 0;
}

void ldr_dma_unmap(struct ldr_dma_mapping *mapping)
{
    if (mapping) {
        memset(mapping, 0, sizeof(*mapping));
    }
}

static struct vfs_node *ldr_find_firmware_node(const char *name, char *path, size_t path_size)
{
    ksnprintf(path, path_size, "/lib/firmware/iwlwifi/%s", name);
    struct vfs_node *node = vfs_lookup(path, "/");
    if (node && node->type == VFS_NODE_FILE && node->data && node->size) {
        return node;
    }
    ksnprintf(path, path_size, "/lib/firmware/%s", name);
    node = vfs_lookup(path, "/");
    if (node && node->type == VFS_NODE_FILE && node->data && node->size) {
        return node;
    }
    return NULL;
}

int ldr_request_firmware(const char *name, struct ldr_firmware *out)
{
    if (!name || !out) {
        return LDR_ERR_INVALID;
    }
    char path[VFS_PATH_MAX];
    struct vfs_node *node = ldr_find_firmware_node(name, path, sizeof(path));
    ldr_note_firmware_request();
    if (!node) {
        log_warn("linuxdrv", "firmware %s not found", name);
        return LDR_ERR_NOTFOUND;
    }
    out->name = name;
    out->data = node->data;
    out->size = node->size;
    log_info("linuxdrv", "firmware %s loaded from %s (%llu bytes)",
             name, path, (uint64_t)node->size);
    return 0;
}

void ldr_release_firmware(struct ldr_firmware *fw)
{
    if (fw) {
        memset(fw, 0, sizeof(*fw));
    }
}

int ldr_cfg80211_scan(struct net_iface *iface)
{
    ldr_note_cfg80211_op();
    if (!iface || !ldr_iface_is_linux_wifi(iface)) {
        return LDR_ERR_NOTFOUND;
    }
    log_warn("linuxdrv", "%s cfg80211 scan requested, but Linux .ko cfg80211_ops are not bound yet",
             iface->name);
    return LDR_ERR_UNSUPPORTED;
}

int ldr_cfg80211_connect(struct net_iface *iface, const char *ssid, const char *passphrase)
{
    (void)passphrase;
    ldr_note_cfg80211_op();
    if (!iface || !ldr_iface_is_linux_wifi(iface) || !ssid || !ssid[0]) {
        return LDR_ERR_INVALID;
    }
    log_warn("linuxdrv", "%s cfg80211 connect '%s' requested, but Linux .ko cfg80211_ops are not bound yet",
             iface->name, ssid);
    return LDR_ERR_UNSUPPORTED;
}

int ldr_cfg80211_disconnect(struct net_iface *iface)
{
    ldr_note_cfg80211_op();
    if (!iface || !ldr_iface_is_linux_wifi(iface)) {
        return LDR_ERR_NOTFOUND;
    }
    iface->link = false;
    iface->up = false;
    return 0;
}

int ldr_mac80211_register_hw(struct net_iface *iface)
{
    ldr_note_mac80211_op();
    if (!iface || !ldr_iface_is_linux_wifi(iface)) {
        return LDR_ERR_NOTFOUND;
    }
    log_warn("linuxdrv", "%s mac80211 ieee80211_hw registration waiting for Linux module symbol ABI",
             iface->name);
    return LDR_ERR_UNSUPPORTED;
}

int ldr_i915_probe(const struct pci_device *dev)
{
    ldr_note_drm_event();
    if (!dev || dev->vendor_id != 0x8086 || dev->class_code != 0x03) {
        return LDR_ERR_NOTFOUND;
    }

    /* Initialize DRM/KMS stub for Intel GPU */
    int rc = drm_i915_probe(dev->vendor_id, dev->device_id);
    if (rc < 0) {
        log_warn("linuxdrv", "Intel GPU %04x i915 probe failed, using legacy framebuffer",
                 dev->device_id);
        return LDR_ERR_UNSUPPORTED;
    }

    log_info("linuxdrv", "Intel GPU %04x: i915 KMS stub active (full 3D accel pending)",
             dev->device_id);
    return 0; /* Success - KMS stub is now active */
}
