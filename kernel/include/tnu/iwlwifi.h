#ifndef TNU_IWLWIFI_H
#define TNU_IWLWIFI_H

#include <arch/pci.h>
#include <tnu/net.h>
#include <tnu/types.h>

struct iwlwifi_fw_part {
    const uint8_t *text;
    uint32_t text_size;
    const uint8_t *data;
    uint32_t data_size;
};

#define IWLWIFI_AP_CACHE_MAX 16

struct iwlwifi_ap {
    bool valid;
    char ssid[33];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    bool privacy;
};

struct iwlwifi_state {
    uintptr_t mmio;
    uint16_t device_id;
    uint32_t hw_rev;
    uint32_t rf_id;
    uint32_t gp_cntrl;
    bool attached;
    bool firmware_loaded;
    bool firmware_parsed;
    bool firmware_staged;
    bool firmware_running;
    bool firmware_alive;
    bool firmware_tlv;
    bool modern_transport;
    bool associated;
    bool link_ready;
    bool scanning;
    bool auth_done;
    bool assoc_done;
    uint16_t auth_status;
    uint16_t assoc_status;
    uint16_t assoc_id;
    uint16_t tx_sequence;
    bool wpa_key_msg1;
    bool wpa_key_msg3;
    bool wpa_ptk_ready;
    uint64_t wpa_replay_counter;
    uint8_t wpa_pmk[32];
    uint8_t wpa_ptk[64];
    uint8_t wpa_anonce[32];
    uint8_t wpa_snonce[32];
    struct net_iface *iface;
    char associated_ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    struct iwlwifi_ap aps[IWLWIFI_AP_CACHE_MAX];
    size_t ap_count;
    const uint8_t *firmware_data;
    size_t firmware_size;
    uint32_t firmware_version;
    uint32_t firmware_build;
    char firmware_human[65];
    struct iwlwifi_fw_part init_ucode;
    struct iwlwifi_fw_part runtime_ucode;
    struct iwlwifi_fw_part boot_ucode;
    void *firmware_dma;
    uintptr_t firmware_dma_phys;
    size_t firmware_dma_size;
    uint32_t *rx_desc;
    uintptr_t rx_desc_phys;
    void *rx_status;
    uintptr_t rx_status_phys;
    uint8_t *rx_buffers;
    uintptr_t rx_buffers_phys;
    size_t rx_dma_size;
    uint16_t rx_index;
    void *cmd_desc;
    uintptr_t cmd_desc_phys;
    void *cmd_ring;
    uintptr_t cmd_ring_phys;
    size_t cmd_dma_size;
    uint16_t cmd_index;
    bool cmd_queue_ready;
    bool command_done;
    uint8_t command_done_code;
    net_rx_callback_t rx_callback;
    void *rx_callback_ctx;
    void *tx_desc;
    uintptr_t tx_desc_phys;
    void *tx_ring;
    uintptr_t tx_ring_phys;
    size_t tx_dma_size;
    uint16_t tx_index;
    bool tx_queue_ready;
    const char *family;
    const char *firmware_name;
};

bool iwlwifi_is_supported(const struct pci_device *dev);
int iwlwifi_attach(struct net_iface *iface, const struct pci_device *dev);
int iwlwifi_start(struct net_iface *iface);
int iwlwifi_scan(struct net_iface *iface);
int iwlwifi_associate(struct net_iface *iface, const char *ssid, const char *passphrase);
const struct iwlwifi_state *iwlwifi_state_for(const struct net_iface *iface);

#endif
