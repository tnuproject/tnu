#ifndef TNU_LINUX_DRIVER_RUNTIME_H
#define TNU_LINUX_DRIVER_RUNTIME_H

#include <tnu/net.h>
#include <tnu/types.h>

struct pci_device;

typedef void (*ldr_work_fn_t)(void *ctx);
typedef void (*ldr_irq_handler_t)(void *ctx);

enum ldr_module_state {
    LDR_MODULE_ABSENT = 0,
    LDR_MODULE_DISCOVERED,
    LDR_MODULE_LOADED,
    LDR_MODULE_PROBED,
    LDR_MODULE_ATTACHED,
    LDR_MODULE_RUNNING,
    LDR_MODULE_RECOVERY,
    LDR_MODULE_UNLOADED,
    LDR_MODULE_FAILED,
};

enum ldr_device_state {
    LDR_DEVICE_EMPTY = 0,
    LDR_DEVICE_CLAIMED,
    LDR_DEVICE_EXPOSED,
    LDR_DEVICE_PROBED,
    LDR_DEVICE_ATTACHED,
    LDR_DEVICE_RUNNING,
    LDR_DEVICE_RECOVERY,
    LDR_DEVICE_FAILED,
};

enum ldr_bus_type {
    LDR_BUS_PCI = 1,
};

enum ldr_backend_kind {
    LDR_BACKEND_NETDEV = 1,
    LDR_BACKEND_WIFI,
    LDR_BACKEND_DRM,
};

struct ldr_module_info {
    char name[32];
    enum ldr_module_state state;
    const char *role;
    const char *requires;
    uint32_t load_order;
    uint32_t refcount;
    int last_error;
};

struct ldr_device_info {
    char native_name[NET_NAME_MAX + 1];
    char linux_name[16];
    enum ldr_bus_type bus;
    enum ldr_backend_kind backend;
    enum ldr_device_state state;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus_id;
    uint8_t slot;
    uint8_t function;
    const char *module;
    int last_error;
};

struct ldr_stats {
    uint32_t modules;
    uint32_t devices;
    uint32_t module_load_attempts;
    uint32_t pci_claims;
    uint32_t irq_redirects;
    uint32_t dma_maps;
    uint32_t firmware_requests;
    uint32_t work_items;
    uint32_t cfg80211_ops;
    uint32_t mac80211_ops;
    uint32_t drm_events;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t scan_requests;
    uint32_t connect_requests;
    uint32_t recovery_events;
    int last_error;
};

struct ldr_work_item {
    const char *name;
    ldr_work_fn_t fn;
    void *ctx;
};

struct ldr_dma_mapping {
    void *cpu_addr;
    uintptr_t dma_addr;
    size_t size;
    bool streaming;
};

struct ldr_firmware {
    const char *name;
    const void *data;
    size_t size;
};

void ldr_init(void);
int ldr_register_builtin_modules(void);
int ldr_load_module(const char *name);
int ldr_load_module_image(const char *name, const void *image, size_t size);
int ldr_probe_pci_netdev(struct net_iface *iface, const struct pci_device *dev);
bool ldr_iface_is_linux_wifi(const struct net_iface *iface);
int ldr_wifi_scan(struct net_iface *iface);
int ldr_wifi_connect(struct net_iface *iface, const char *ssid, const char *passphrase);
int ldr_wifi_status(const struct net_iface *iface, struct wifi_status *out);
int ldr_wifi_scan_results(const struct net_iface *iface, struct wifi_ap *out, size_t max_aps);
void ldr_get_stats(struct ldr_stats *out);
size_t ldr_module_count(void);
const struct ldr_module_info *ldr_module_get(size_t index);
size_t ldr_device_count(void);
const struct ldr_device_info *ldr_device_get(size_t index);
const char *ldr_module_state_name(enum ldr_module_state state);
const char *ldr_device_state_name(enum ldr_device_state state);
void ldr_print_modules(void);
void ldr_print_stats(void);
void ldr_print_logs(void);
void ldr_print_net_trace(void);
void ldr_note_irq_redirect(void);
void ldr_note_dma_map(void);
void ldr_note_firmware_request(void);
void ldr_note_work_item(void);
void ldr_note_cfg80211_op(void);
void ldr_note_mac80211_op(void);
void ldr_note_drm_event(void);

int ldr_queue_work(const char *name, ldr_work_fn_t fn, void *ctx);
void ldr_run_workqueues(void);
int ldr_request_irq(uint8_t irq, ldr_irq_handler_t handler, void *ctx, const char *name);
int ldr_free_irq(uint8_t irq, void *ctx);
int ldr_dispatch_irq(uint8_t irq);
int ldr_dma_map(void *cpu_addr, size_t size, bool streaming, struct ldr_dma_mapping *out);
void ldr_dma_unmap(struct ldr_dma_mapping *mapping);
int ldr_request_firmware(const char *name, struct ldr_firmware *out);
void ldr_release_firmware(struct ldr_firmware *fw);
int ldr_cfg80211_scan(struct net_iface *iface);
int ldr_cfg80211_connect(struct net_iface *iface, const char *ssid, const char *passphrase);
int ldr_cfg80211_disconnect(struct net_iface *iface);
int ldr_mac80211_register_hw(struct net_iface *iface);
int ldr_i915_probe(const struct pci_device *dev);

#endif
