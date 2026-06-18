#include <arch/pci.h>
#include <tnu/linux_driver_runtime.h>
#include <tnu/log.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/vfs.h>

#define LDR_MAX_MODULES 16
#define LDR_MAX_DEVICES 8
#define LDR_ERR_UNSUPPORTED (-95)
#define LDR_ERR_BUSY        (-16)
#define LDR_ERR_NOTFOUND    (-2)
#define LDR_ERR_INVALID     (-22)

struct ldr_module_slot {
    struct ldr_module_info info;
};

struct ldr_device_slot {
    struct ldr_device_info info;
    struct net_iface *iface;
    const struct pci_device *pci;
};

static struct ldr_module_slot ldr_modules[LDR_MAX_MODULES];
static struct ldr_device_slot ldr_devices[LDR_MAX_DEVICES];
static struct ldr_stats ldr_stats_data;
static bool ldr_ready;

static const struct ldr_module_info ldr_builtin_modules[] = {
    { "cfg80211", LDR_MODULE_DISCOVERED, "Linux wireless regulatory/cfg API", "linux-core", 10, 0, 0 },
    { "mac80211", LDR_MODULE_DISCOVERED, "Linux 802.11 softmac stack", "cfg80211", 20, 0, 0 },
    { "iwlwifi", LDR_MODULE_DISCOVERED, "Intel PCI Wi-Fi transport", "cfg80211", 30, 0, 0 },
    { "iwlmvm", LDR_MODULE_DISCOVERED, "Intel MVM firmware driver", "iwlwifi mac80211", 40, 0, 0 },
    { "drm", LDR_MODULE_DISCOVERED, "Linux DRM core bridge", "linux-core", 50, 0, 0 },
    { "i915", LDR_MODULE_DISCOVERED, "Intel iGPU DRM driver", "drm", 60, 0, 0 },
};

const char *ldr_module_state_name(enum ldr_module_state state)
{
    switch (state) {
    case LDR_MODULE_ABSENT: return "ABSENT";
    case LDR_MODULE_DISCOVERED: return "DISCOVERED";
    case LDR_MODULE_LOADED: return "LOADED";
    case LDR_MODULE_PROBED: return "PROBED";
    case LDR_MODULE_ATTACHED: return "ATTACHED";
    case LDR_MODULE_RUNNING: return "RUNNING";
    case LDR_MODULE_RECOVERY: return "RECOVERY";
    case LDR_MODULE_UNLOADED: return "UNLOADED";
    case LDR_MODULE_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

const char *ldr_device_state_name(enum ldr_device_state state)
{
    switch (state) {
    case LDR_DEVICE_EMPTY: return "EMPTY";
    case LDR_DEVICE_CLAIMED: return "CLAIMED";
    case LDR_DEVICE_EXPOSED: return "EXPOSED";
    case LDR_DEVICE_PROBED: return "PROBED";
    case LDR_DEVICE_ATTACHED: return "ATTACHED";
    case LDR_DEVICE_RUNNING: return "RUNNING";
    case LDR_DEVICE_RECOVERY: return "RECOVERY";
    case LDR_DEVICE_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

static struct ldr_module_info *ldr_find_module(const char *name)
{
    for (size_t i = 0; i < LDR_MAX_MODULES; i++) {
        if (ldr_modules[i].info.name[0] && strcmp(ldr_modules[i].info.name, name) == 0) {
            return &ldr_modules[i].info;
        }
    }
    return NULL;
}

static struct ldr_device_slot *ldr_find_device_by_iface(const struct net_iface *iface)
{
    for (size_t i = 0; i < LDR_MAX_DEVICES; i++) {
        if (ldr_devices[i].iface == iface) {
            return &ldr_devices[i];
        }
    }
    return NULL;
}

static struct ldr_device_slot *ldr_find_device_by_pci(const struct pci_device *dev)
{
    for (size_t i = 0; i < LDR_MAX_DEVICES; i++) {
        if (ldr_devices[i].pci == dev) {
            return &ldr_devices[i];
        }
    }
    return NULL;
}

static struct vfs_node *ldr_find_module_node(const char *name, char *path, size_t path_size)
{
    const char *prefixes[] = {
        "/lib/modules",
        "/usr/linux/lib/modules",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        ksnprintf(path, path_size, "%s/%s.ko", prefixes[i], name);
        struct vfs_node *node = vfs_lookup(path, "/");
        if (node && node->type == VFS_NODE_FILE && node->data && node->size) {
            return node;
        }
    }
    return NULL;
}

static int ldr_load_module_dependencies(struct ldr_module_info *module)
{
    if (!module || !module->requires || !module->requires[0]) {
        return 0;
    }
    const char *p = module->requires;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        char dep[32];
        size_t n = 0;
        while (*p && *p != ' ' && n + 1 < sizeof(dep)) {
            dep[n++] = *p++;
        }
        dep[n] = '\0';
        if (strcmp(dep, "linux-core") == 0) {
            continue;
        }
        if (!ldr_find_module(dep)) {
            continue;
        }
        int rc = ldr_load_module(dep);
        if (rc < 0) {
            module->last_error = rc;
            return rc;
        }
    }
    return 0;
}

void ldr_init(void)
{
    memset(ldr_modules, 0, sizeof(ldr_modules));
    memset(ldr_devices, 0, sizeof(ldr_devices));
    memset(&ldr_stats_data, 0, sizeof(ldr_stats_data));
    ldr_ready = true;
    ldr_register_builtin_modules();
}

int ldr_register_builtin_modules(void)
{
    size_t n = sizeof(ldr_builtin_modules) / sizeof(ldr_builtin_modules[0]);
    for (size_t i = 0; i < n && i < LDR_MAX_MODULES; i++) {
        ldr_modules[i].info = ldr_builtin_modules[i];
    }
    ldr_stats_data.modules = (uint32_t)n;
    return 0;
}

int ldr_load_module(const char *name)
{
    struct ldr_module_info *module = ldr_find_module(name);
    ldr_stats_data.module_load_attempts++;
    if (!module) {
        ldr_stats_data.last_error = LDR_ERR_NOTFOUND;
        return LDR_ERR_NOTFOUND;
    }
    if (module->state == LDR_MODULE_LOADED || module->state == LDR_MODULE_PROBED ||
        module->state == LDR_MODULE_ATTACHED || module->state == LDR_MODULE_RUNNING) {
        module->refcount++;
        return 0;
    }

    int dep_rc = ldr_load_module_dependencies(module);
    if (dep_rc < 0) {
        ldr_stats_data.last_error = dep_rc;
        return dep_rc;
    }

    char path[VFS_PATH_MAX];
    struct vfs_node *node = ldr_find_module_node(name, path, sizeof(path));
    if (!node) {
        module->state = LDR_MODULE_DISCOVERED;
        module->last_error = LDR_ERR_NOTFOUND;
        ldr_stats_data.last_error = LDR_ERR_NOTFOUND;
        log_warn("linuxdrv", "module %s.ko not found under /lib/modules or /usr/linux/lib/modules", name);
        return LDR_ERR_NOTFOUND;
    }

    int rc = ldr_load_module_image(name, node->data, (size_t)node->size);
    if (rc == 0) {
        log_info("linuxdrv", "module %s staged from %s", name, path);
    }
    return rc;
}

int ldr_load_module_image(const char *name, const void *image, size_t size)
{
    const uint8_t *elf = image;
    struct ldr_module_info *module = ldr_find_module(name);
    ldr_stats_data.module_load_attempts++;
    if (!module) {
        ldr_stats_data.last_error = LDR_ERR_NOTFOUND;
        return LDR_ERR_NOTFOUND;
    }
    if (!elf || size < 64 || elf[0] != 0x7f || elf[1] != 'E' ||
        elf[2] != 'L' || elf[3] != 'F') {
        module->state = LDR_MODULE_FAILED;
        module->last_error = LDR_ERR_INVALID;
        ldr_stats_data.last_error = LDR_ERR_INVALID;
        return LDR_ERR_INVALID;
    }
    if (elf[4] != 2 || elf[5] != 1) {
        module->state = LDR_MODULE_FAILED;
        module->last_error = LDR_ERR_UNSUPPORTED;
        ldr_stats_data.last_error = LDR_ERR_UNSUPPORTED;
        return LDR_ERR_UNSUPPORTED;
    }
    uint16_t type = (uint16_t)elf[16] | ((uint16_t)elf[17] << 8);
    uint16_t machine = (uint16_t)elf[18] | ((uint16_t)elf[19] << 8);
    if (type != 1 || machine != 62) {
        module->state = LDR_MODULE_FAILED;
        module->last_error = LDR_ERR_UNSUPPORTED;
        ldr_stats_data.last_error = LDR_ERR_UNSUPPORTED;
        return LDR_ERR_UNSUPPORTED;
    }
    module->state = LDR_MODULE_LOADED;
    module->last_error = 0;
    module->refcount++;
    return 0;
}

int ldr_probe_pci_netdev(struct net_iface *iface, const struct pci_device *dev)
{
    if (!ldr_ready) {
        ldr_init();
    }
    if (!iface || !dev) {
        return -1;
    }
    if (ldr_find_device_by_pci(dev)) {
        return LDR_ERR_BUSY;
    }

    struct ldr_device_slot *slot = NULL;
    for (size_t i = 0; i < LDR_MAX_DEVICES; i++) {
        if (ldr_devices[i].info.state == LDR_DEVICE_EMPTY) {
            slot = &ldr_devices[i];
            break;
        }
    }
    if (!slot) {
        return LDR_ERR_BUSY;
    }

    memset(slot, 0, sizeof(*slot));
    slot->iface = iface;
    slot->pci = dev;
    strncpy(slot->info.native_name, iface->name, sizeof(slot->info.native_name) - 1);
    strncpy(slot->info.linux_name, "wlan0", sizeof(slot->info.linux_name) - 1);
    slot->info.bus = LDR_BUS_PCI;
    slot->info.backend = LDR_BACKEND_WIFI;
    slot->info.state = LDR_DEVICE_EXPOSED;
    slot->info.vendor_id = dev->vendor_id;
    slot->info.device_id = dev->device_id;
    slot->info.bus_id = dev->bus;
    slot->info.slot = dev->slot;
    slot->info.function = dev->function;
    slot->info.module = "iwlmvm";
    slot->info.last_error = LDR_ERR_UNSUPPORTED;

    iface->driver = "linux-iwlwifi";
    iface->driver_data = slot;
    iface->up = false;
    iface->link = false;

    ldr_stats_data.devices++;
    ldr_stats_data.pci_claims++;
    ldr_stats_data.last_error = LDR_ERR_UNSUPPORTED;

    ldr_load_module("cfg80211");
    ldr_load_module("mac80211");
    ldr_load_module("iwlwifi");
    ldr_load_module("iwlmvm");

    log_warn("linuxdrv", "%s exposed PCI %04x:%04x to Linux iwlwifi runtime; .ko ABI not live yet",
             iface->name, dev->vendor_id, dev->device_id);
    return 0;
}

bool ldr_iface_is_linux_wifi(const struct net_iface *iface)
{
    return iface && iface->type == NET_IFACE_WIFI &&
           iface->driver && strcmp(iface->driver, "linux-iwlwifi") == 0;
}

int ldr_wifi_scan(struct net_iface *iface)
{
    struct ldr_device_slot *slot = ldr_find_device_by_iface(iface);
    ldr_stats_data.scan_requests++;
    if (!slot) {
        return LDR_ERR_NOTFOUND;
    }
    slot->info.last_error = LDR_ERR_UNSUPPORTED;
    ldr_stats_data.last_error = LDR_ERR_UNSUPPORTED;
    return ldr_cfg80211_scan(iface);
}

int ldr_wifi_connect(struct net_iface *iface, const char *ssid, const char *passphrase)
{
    (void)ssid;
    (void)passphrase;
    struct ldr_device_slot *slot = ldr_find_device_by_iface(iface);
    ldr_stats_data.connect_requests++;
    if (!slot) {
        return LDR_ERR_NOTFOUND;
    }
    slot->info.last_error = LDR_ERR_UNSUPPORTED;
    ldr_stats_data.last_error = LDR_ERR_UNSUPPORTED;
    return ldr_cfg80211_connect(iface, ssid, passphrase);
}

int ldr_wifi_status(const struct net_iface *iface, struct wifi_status *out)
{
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    return ldr_find_device_by_iface(iface) ? 0 : LDR_ERR_NOTFOUND;
}

int ldr_wifi_scan_results(const struct net_iface *iface, struct wifi_ap *out, size_t max_aps)
{
    (void)out;
    (void)max_aps;
    return ldr_find_device_by_iface(iface) ? 0 : LDR_ERR_NOTFOUND;
}

void ldr_get_stats(struct ldr_stats *out)
{
    if (out) {
        *out = ldr_stats_data;
    }
}

void ldr_note_irq_redirect(void) { ldr_stats_data.irq_redirects++; }
void ldr_note_dma_map(void) { ldr_stats_data.dma_maps++; }
void ldr_note_firmware_request(void) { ldr_stats_data.firmware_requests++; }
void ldr_note_work_item(void) { ldr_stats_data.work_items++; }
void ldr_note_cfg80211_op(void) { ldr_stats_data.cfg80211_ops++; }
void ldr_note_mac80211_op(void) { ldr_stats_data.mac80211_ops++; }
void ldr_note_drm_event(void) { ldr_stats_data.drm_events++; }

size_t ldr_module_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < LDR_MAX_MODULES; i++) {
        if (ldr_modules[i].info.name[0]) {
            count++;
        }
    }
    return count;
}

const struct ldr_module_info *ldr_module_get(size_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < LDR_MAX_MODULES; i++) {
        if (!ldr_modules[i].info.name[0]) {
            continue;
        }
        if (seen++ == index) {
            return &ldr_modules[i].info;
        }
    }
    return NULL;
}

size_t ldr_device_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < LDR_MAX_DEVICES; i++) {
        if (ldr_devices[i].info.state != LDR_DEVICE_EMPTY) {
            count++;
        }
    }
    return count;
}

const struct ldr_device_info *ldr_device_get(size_t index)
{
    size_t seen = 0;
    for (size_t i = 0; i < LDR_MAX_DEVICES; i++) {
        if (ldr_devices[i].info.state == LDR_DEVICE_EMPTY) {
            continue;
        }
        if (seen++ == index) {
            return &ldr_devices[i].info;
        }
    }
    return NULL;
}

void ldr_print_modules(void)
{
    kprintf("Linux Driver Runtime modules:\n");
    for (size_t i = 0; i < ldr_module_count(); i++) {
        const struct ldr_module_info *m = ldr_module_get(i);
        kprintf("  %s state=%s order=%u role=%s requires=%s last=%d\n",
                m->name, ldr_module_state_name(m->state), m->load_order,
                m->role ? m->role : "-", m->requires ? m->requires : "-", m->last_error);
    }
}

void ldr_print_stats(void)
{
    struct ldr_stats st;
    ldr_get_stats(&st);
    kprintf("Linux Driver Runtime stats:\n");
    kprintf("  modules=%u devices=%u load_attempts=%u pci_claims=%u\n",
            st.modules, st.devices, st.module_load_attempts, st.pci_claims);
    kprintf("  irq=%u dma=%u firmware=%u work=%u cfg80211=%u mac80211=%u drm=%u\n",
            st.irq_redirects, st.dma_maps, st.firmware_requests, st.work_items,
            st.cfg80211_ops, st.mac80211_ops, st.drm_events);
    kprintf("  scan=%u connect=%u tx=%u rx=%u recovery=%u last_error=%d\n",
            st.scan_requests, st.connect_requests, st.tx_packets, st.rx_packets,
            st.recovery_events, st.last_error);
    for (size_t i = 0; i < ldr_device_count(); i++) {
        const struct ldr_device_info *d = ldr_device_get(i);
        kprintf("  %s<-%s pci=%02x:%02x.%u %04x:%04x module=%s state=%s last=%d\n",
                d->native_name, d->linux_name, d->bus_id, d->slot, d->function,
                d->vendor_id, d->device_id, d->module ? d->module : "-",
                ldr_device_state_name(d->state), d->last_error);
    }
}

void ldr_print_logs(void)
{
    kprintf("linuxdrv logs:\n");
    kprintf("  LDR initialized as an in-kernel compatibility provider manager.\n");
    kprintf("  ELF64 relocatable module images can be validated/staged with ldr_load_module_image.\n");
    kprintf("  Workqueue, IRQ, DMA, firmware, cfg80211/mac80211 and i915 bridge shims are present.\n");
    kprintf("  Real Linux .ko execution is gated on relocation, exported symbols and init trampolines.\n");
    kprintf("  Devices exposed here are not double-owned by native drivers.\n");
}

void ldr_print_net_trace(void)
{
    kprintf("net trace:\n");
    kprintf("  native socket -> Tiramisu net stack -> TNAI -> Linux net_device -> driver -> hardware\n");
    kprintf("  hardware -> Linux driver -> net_device RX -> TNAI -> Tiramisu net stack -> native sockets\n");
    kprintf("  current packet bridge state: scaffolding active, Linux .ko data path not live\n");
}
