#include <arch/pci.h>
#include <arch/pit.h>
#include <tnu/crypto.h>
#include <tnu/iwlwifi.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/vfs.h>

/*
 * Native TNU iwlwifi foundation.
 *
 * This is intentionally staged: it performs PCI attach, BAR mapping, interrupt
 * quiescing, hardware identity reads, and firmware discovery/validation. The
 * next steps are firmware DMA upload, command queues, RX/TX rings,
 * mac80211/net80211 semantics, regulatory data, and WPA association.
 *
 * The register names and attach order are informed by the public FreeBSD
 * iwlwifi port and Intel's Linux iwlwifi driver, but this file is original TNU
 * code.
 */

#define IWL_MAX_DEVICES 8
#define IWL_MMIO_SIZE   (128 * 1024)
#define IWL_FW_DMA_SIZE (512 * 1024)
#define IWL_RX_RING_LOG 6
#define IWL_RX_RING_COUNT (1u << IWL_RX_RING_LOG)
#define IWL_RX_BUF_SIZE 4096
#define IWL_RX_DMA_SIZE (PAGE_SIZE * 2 + IWL_RX_RING_COUNT * IWL_RX_BUF_SIZE)
#define IWL_TX_RING_COUNT 256
#define IWL_TX_DESC_SIZE 128
#define IWL_CMD_QUEUE 4
#define IWL_MGMT_QUEUE 0
#define IWL_CMD_DATA_SIZE 1024
#define IWL_CMD_DMA_SIZE (IWL_TX_RING_COUNT * IWL_TX_DESC_SIZE + \
                          IWL_TX_RING_COUNT * (4 + IWL_CMD_DATA_SIZE))

#define CSR_HW_IF_CONFIG_REG 0x000
#define CSR_INT              0x008
#define CSR_INT_MASK         0x00c
#define CSR_FH_INT_STATUS    0x010
#define CSR_RESET            0x020
#define CSR_GP_CNTRL         0x024
#define CSR_HW_REV           0x028
#define CSR_GIO              0x03c
#define CSR_GP_DRIVER        0x050
#define CSR_UCODE_GP1_CLR    0x05c
#define CSR_SHADOW_REG_CTRL  0x0a8
#define CSR_GIO_CHICKEN      0x100
#define CSR_DBG_HPET_MEM     0x240
#define CSR_HW_RF_ID         0x09c
#define CSR_PRPH_WADDR       0x444
#define CSR_PRPH_RADDR       0x448
#define CSR_PRPH_WDATA       0x44c
#define CSR_PRPH_RDATA       0x450
#define CSR_HBUS_TARG_WRPTR  0x460

#define FH_TFBD_CTRL0(qid)   (0x1900 + (qid) * 8)
#define FH_TFBD_CTRL1(qid)   (0x1904 + (qid) * 8)
#define FH_KW_ADDR           0x197c
#define FH_SRAM_ADDR(qid)    (0x19a4 + (qid) * 4)
#define FH_CBBC_QUEUE(qid)   (0x19d0 + (qid) * 4)
#define FH_STATUS_WPTR       0x1bc0
#define FH_RX_BASE           0x1bc4
#define FH_RX_WPTR           0x1bc8
#define FH_RX_CONFIG         0x1c00
#define FH_TXBUF_STATUS(qid) (0x1d08 + (qid) * 32)
#define FH_TX_CONFIG(qid)    (0x1d00 + (qid) * 32)

#define IWL_TLV_UCODE_MAGIC 0x0a4c5749u
#define IWL_TLV_HUMAN_SIZE  64
#define IWL_TLV_HEADER_SIZE  88

#define IWL_TLV_MAIN_TEXT 1
#define IWL_TLV_MAIN_DATA 2
#define IWL_TLV_INIT_TEXT 3
#define IWL_TLV_INIT_DATA 4
#define IWL_TLV_BOOT_TEXT 5

#define IWL_RX_TYPE_UC_READY       1
#define IWL_RX_TYPE_ADD_NODE_DONE  24
#define IWL_RX_TYPE_TX_DONE        28
#define IWL_RX_TYPE_START_SCAN     130
#define IWL_RX_TYPE_SCAN_RESULT    131
#define IWL_RX_TYPE_STOP_SCAN      132
#define IWL_RX_TYPE_STATE_CHANGED  161
#define IWL_RX_TYPE_RX_PHY         192
#define IWL_RX_TYPE_MPDU_RX_DONE   193
#define IWL_RX_DESC_QID_MASK       0x1f
#define IWL_RX_UNSOLICITED         0x80

#define IWL_CMD_RXON          16
#define IWL_CMD_RXON_ASSOC    17
#define IWL_CMD_ADD_NODE      24
#define IWL_CMD_TX_DATA       28
#define IWL_CMD_LINK_QUALITY  78
#define IWL_CMD_SET_POWER     119
#define IWL_CMD_SCAN          128
#define IWL_CMD_GET_STATISTICS 156

#define IWL_RX_NO_CRC_ERR     (1u << 0)
#define IWL_RX_NO_OVFL_ERR    (1u << 1)
#define IWL_RX_NOERROR        (IWL_RX_NO_CRC_ERR | IWL_RX_NO_OVFL_ERR)

#define IWL_MODE_STA          3
#define IWL_RXCHAIN_DRIVER_FORCE       (1u << 0)
#define IWL_RXCHAIN_VALID(x)           (((x) & 7u) << 1)
#define IWL_RXCHAIN_FORCE_SEL(x)       (((x) & 7u) << 4)
#define IWL_RXCHAIN_FORCE_MIMO_SEL(x)  (((x) & 7u) << 7)
#define IWL_RXCHAIN_IDLE_COUNT(x)      ((x) << 10)
#define IWL_RXCHAIN_MIMO_COUNT(x)      ((x) << 12)
#define IWL_RXON_24GHZ        (1u << 0)
#define IWL_RXON_CCK          (1u << 1)
#define IWL_RXON_AUTO         (1u << 2)
#define IWL_RXON_TSF          (1u << 15)
#define IWL_RXON_CTS_TO_SELF  (1u << 30)
#define IWL_FILTER_MULTICAST  (1u << 2)
#define IWL_FILTER_NODECRYPT  (1u << 3)
#define IWL_FILTER_BSS        (1u << 5)
#define IWL_FILTER_BEACON     (1u << 6)
#define IWL_CHAN_PASSIVE      0u
#define IWL_CHAN_ACTIVE       1u
#define IWL_CHAN_NPBREQS(x)   (((1u << (x)) - 1u) << 1)
#define IWL_SCAN_CRC_NEVER    0xffffu
#define IWL_TX_AUTO_SEQ       (1u << 13)
#define IWL_TX_NEED_ACK       (1u << 3)
#define IWL_TX_NEED_PADDING   (1u << 20)
#define IWL_RFLAG_CCK         (1u << 9)
#define IWL_RFLAG_ANT(x)      ((x) << 14)
#define IWL_LIFETIME_INFINITE 0xffffffffu

#define IEEE80211_FC0_TYPE_MASK        0x0c
#define IEEE80211_FC0_TYPE_MGT         0x00
#define IEEE80211_FC0_TYPE_DATA        0x08
#define IEEE80211_FC1_DIR_TODS         0x01
#define IEEE80211_FC1_DIR_FROMDS       0x02
#define IEEE80211_FC1_PROTECTED        0x40
#define IEEE80211_FC0_SUBTYPE_MASK     0xf0
#define IEEE80211_FC0_SUBTYPE_PROBE_REQ  0x40
#define IEEE80211_FC0_SUBTYPE_PROBE_RESP 0x50
#define IEEE80211_FC0_SUBTYPE_AUTH       0xb0
#define IEEE80211_FC0_SUBTYPE_ASSOC_REQ  0x00
#define IEEE80211_FC0_SUBTYPE_ASSOC_RESP 0x10
#define IEEE80211_FC0_SUBTYPE_BEACON     0x80
#define IEEE80211_CAPINFO_PRIVACY      0x0010
#define IEEE80211_CAPINFO_ESS          0x0001
#define IEEE80211_STATUS_SUCCESS       0
#define IEEE80211_ELEMID_SSID          0
#define IEEE80211_ELEMID_RATES         1
#define IEEE80211_ELEMID_DSPARMS       3
#define IEEE80211_ELEMID_RSN           48
#define IEEE80211_ADDR_LEN             6
#define IEEE80211_AUTH_ALG_OPEN        0
#define IEEE80211_AUTH_TRANSACTION_REQ 1
#define IEEE80211_AUTH_TRANSACTION_RESP 2

#define ETH_TYPE_EAPOL                 0x888e
#define EAPOL_VERSION_2                2
#define EAPOL_TYPE_KEY                 3
#define RSN_KEY_DESC                   2
#define WPA_KEY_INFO_TYPE_MASK         0x0007
#define WPA_KEY_INFO_TYPE_HMAC_SHA1_AES 2
#define WPA_KEY_INFO_KEY_TYPE          0x0008
#define WPA_KEY_INFO_INSTALL           0x0040
#define WPA_KEY_INFO_ACK               0x0080
#define WPA_KEY_INFO_MIC               0x0100
#define WPA_KEY_INFO_SECURE            0x0200
#define WPA_KEY_INFO_ENCRYPTED_DATA    0x1000
#define WPA_EAPOL_KEY_BASE_LEN         95

#define IWL_NODE_UPDATE                (1u << 0)
#define IWL_ID_BSS                     0
#define IWL_FLAG_SET_KEY               (1u << 0)
#define IWL_KFLAG_CCMP                 (1u << 1)
#define IWL_KFLAG_MAP                  (1u << 3)

#define IWL_HW_IF_CONFIG_MAC_SI       (1u << 8)
#define IWL_HW_IF_CONFIG_RADIO_SI     (1u << 9)
#define IWL_HW_IF_CONFIG_NIC_READY    (1u << 22)
#define IWL_HW_IF_CONFIG_HAP_WAKE_L1A (1u << 23)
#define IWL_HW_IF_CONFIG_PREPARE_DONE (1u << 25)
#define IWL_HW_IF_CONFIG_PREPARE      (1u << 27)
#define IWL_RESET_LINK_PWR_MGMT_DIS   (1u << 31)
#define IWL_GIO_CHICKEN_L1A_NO_L0S_RX (1u << 23)
#define IWL_GIO_CHICKEN_DIS_L0S_TIMER (1u << 29)
#define IWL_GIO_L0S_ENA               (1u << 1)
#define IWL_PRPH_DWORD ((sizeof(uint32_t) - 1u) << 24)
#define IWL_GP_CNTRL_MAC_ACCESS_ENA (1u << 0)
#define IWL_GP_CNTRL_MAC_CLOCK_READY (1u << 0)
#define IWL_GP_CNTRL_MAC_ACCESS_REQ (1u << 3)
#define IWL_GP_CNTRL_SLEEP          (1u << 4)
#define IWL_GP_CNTRL_RFKILL         (1u << 27)
#define IWL_UCODE_GP1_RFKILL        (1u << 1)
#define IWL_UCODE_GP1_CMD_BLOCKED   (1u << 2)
#define IWL_APMG_CLK_EN             0x3004
#define IWL_APMG_PS                 0x300c
#define IWL_APMG_PCI_STT            0x3010
#define IWL_APMG_DIGITAL_SVR        0x3058
#define IWL_APMG_CLK_CTRL_DMA_CLK_RQT (1u << 9)
#define IWL_APMG_CLK_CTRL_BSM_CLK_RQT (1u << 11)
#define IWL_APMG_PS_EARLY_PWROFF_DIS  (1u << 22)
#define IWL_APMG_PS_PWR_SRC_MASK      (3u << 24)
#define IWL_APMG_PCI_STT_L1A_DIS      (1u << 11)
#define IWL_APMG_DIGITAL_SVR_VOLTAGE_MASK (0xfu << 5)
#define IWL_APMG_DIGITAL_SVR_VOLTAGE_1_32 (3u << 5)
#define IWL_BSM_WR_CTRL             0x3400
#define IWL_FW_TEXT_BASE            0x00000000
#define IWL_FW_DATA_BASE            0x00800000
#define IWL_SRVC_DMACHNL            9
#define IWL_INT_ALIVE               (1u << 0)
#define IWL_INT_SW_ERR              (1u << 25)
#define IWL_INT_FH_TX               (1u << 27)
#define IWL_INT_HW_ERR              (1u << 29)
#define IWL_INT_MASK_MIN            (IWL_INT_ALIVE | IWL_INT_SW_ERR | IWL_INT_FH_TX | IWL_INT_HW_ERR)
#define IWL_FH_INT_TX_CHNL(x)       (1u << (x))
#define IWL_FH_TX_CONFIG_DMA_PAUSE  0
#define IWL_FH_TX_CONFIG_DMA_ENA    (1u << 31)
#define IWL_FH_TX_CONFIG_DMA_CREDIT_ENA (1u << 3)
#define IWL_FH_TX_CONFIG_HOST_END   (1u << 20)
#define IWL_FH_TXBUF_TBNUM(x)       ((x) << 20)
#define IWL_FH_TXBUF_TBIDX(x)       ((x) << 12)
#define IWL_FH_TXBUF_VALID          3
#define IWL_FH_RX_CONFIG_ENA        (1u << 31)
#define IWL_FH_RX_CONFIG_NRBD(x)    ((x) << 20)
#define IWL_FH_RX_CONFIG_SINGLE     (1u << 15)
#define IWL_FH_RX_CONFIG_IRQ_HOST   (1u << 12)
#define IWL_FH_RX_CONFIG_TIMEOUT(x) ((x) << 4)
#define IWL_FH_RX_CONFIG_IGNORE_EMPTY (1u << 2)

struct iwl_tx_desc {
    uint8_t reserved1[3];
    uint8_t nsegs;
    struct {
        uint32_t addr;
        uint16_t len;
    } __attribute__((packed)) segs[20];
    uint32_t reserved2;
} __attribute__((packed));

struct iwl_tx_cmd {
    uint8_t code;
    uint8_t flags;
    uint8_t idx;
    uint8_t qid;
    uint8_t data[IWL_CMD_DATA_SIZE];
} __attribute__((packed));

struct iwl_rx_status {
    uint16_t closed_count;
    uint16_t closed_rx_count;
    uint16_t finished_count;
    uint16_t finished_rx_count;
    uint32_t reserved[2];
} __attribute__((packed));

struct iwl_rx_desc {
    uint32_t len;
    uint8_t type;
    uint8_t flags;
    uint8_t idx;
    uint8_t qid;
} __attribute__((packed));

struct iwl_ucode_info {
    uint8_t minor;
    uint8_t major;
    uint16_t reserved1;
    uint8_t revision[8];
    uint8_t type;
    uint8_t subtype;
    uint16_t reserved2;
    uint32_t logptr;
    uint32_t errptr;
    uint32_t timestamp;
    uint32_t valid;
} __attribute__((packed));

struct iwl_cmd_data {
    uint16_t len;
    uint16_t lnext;
    uint32_t flags;
    uint32_t scratch;
    uint32_t rate;
    uint8_t id;
    uint8_t security;
    uint8_t linkq;
    uint8_t reserved2;
    uint8_t key[16];
    uint16_t fnext;
    uint16_t reserved3;
    uint32_t lifetime;
    uint32_t loaddr;
    uint8_t hiaddr;
    uint8_t rts_ntries;
    uint8_t data_ntries;
    uint8_t tid;
    uint16_t timeout;
    uint16_t txop;
} __attribute__((packed));

struct iwl_scan_essid {
    uint8_t id;
    uint8_t len;
    uint8_t data[32];
} __attribute__((packed));

struct iwl_scan_hdr {
    uint16_t len;
    uint8_t scan_flags;
    uint8_t nchan;
    uint16_t quiet_time;
    uint16_t quiet_threshold;
    uint16_t crc_threshold;
    uint16_t rxchain;
    uint32_t max_svc;
    uint32_t pause_svc;
    uint32_t flags;
    uint32_t filter;
} __attribute__((packed));

struct iwl_scan_chan {
    uint32_t flags;
    uint16_t chan;
    uint8_t rf_gain;
    uint8_t dsp_gain;
    uint16_t active;
    uint16_t passive;
} __attribute__((packed));

struct iwl_rxon {
    uint8_t myaddr[IEEE80211_ADDR_LEN];
    uint16_t reserved1;
    uint8_t bssid[IEEE80211_ADDR_LEN];
    uint16_t reserved2;
    uint8_t wlap[IEEE80211_ADDR_LEN];
    uint16_t reserved3;
    uint8_t mode;
    uint8_t air;
    uint16_t rxchain;
    uint8_t ofdm_mask;
    uint8_t cck_mask;
    uint16_t associd;
    uint32_t flags;
    uint32_t filter;
    uint8_t chan;
    uint8_t reserved4;
    uint8_t ht_single_mask;
    uint8_t ht_dual_mask;
    uint8_t ht_triple_mask;
    uint8_t reserved5;
    uint16_t acquisition;
    uint16_t reserved6;
} __attribute__((packed));

struct iwl_node_info {
    uint8_t control;
    uint8_t reserved1[3];
    uint8_t macaddr[IEEE80211_ADDR_LEN];
    uint16_t reserved2;
    uint8_t id;
    uint8_t flags;
    uint16_t reserved3;
    uint16_t kflags;
    uint8_t tsc2;
    uint8_t reserved4;
    uint16_t ttak[5];
    uint8_t kid;
    uint8_t reserved5;
    uint8_t key[16];
    uint64_t tsc;
    uint8_t rxmic[8];
    uint8_t txmic[8];
    uint32_t htflags;
    uint32_t mask;
    uint16_t disable_tid;
    uint16_t reserved6;
    uint8_t addba_tid;
    uint8_t delba_tid;
    uint16_t addba_ssn;
    uint32_t reserved7;
} __attribute__((packed));

struct iwl_rx_mpdu {
    uint16_t len;
    uint16_t reserved;
} __attribute__((packed));

struct ieee80211_hdr3 {
    uint8_t fc[2];
    uint8_t duration[2];
    uint8_t addr1[IEEE80211_ADDR_LEN];
    uint8_t addr2[IEEE80211_ADDR_LEN];
    uint8_t addr3[IEEE80211_ADDR_LEN];
    uint8_t seq[2];
} __attribute__((packed));

struct iwl_device_info {
    uint16_t id;
    const char *family;
    const char *firmware;
};

static struct iwlwifi_state states[IWL_MAX_DEVICES];
static size_t state_count;

static uint32_t iwl_read32(const struct iwlwifi_state *st, uint32_t reg);
static void iwl_write32(const struct iwlwifi_state *st, uint32_t reg, uint32_t value);
static int iwl_nic_lock(const struct iwlwifi_state *st);
static void iwl_nic_unlock(const struct iwlwifi_state *st);
static void iwl_prph_write(const struct iwlwifi_state *st, uint32_t addr, uint32_t value);
static int iwlwifi_transmit(struct net_iface *iface, const void *frame, size_t len);
static void iwlwifi_poll(struct net_iface *iface, net_rx_callback_t callback, void *ctx);
static int iwl_send_mgmt_frame(struct iwlwifi_state *st, const uint8_t *frame,
                               size_t frame_len, bool need_ack);
static const struct iwlwifi_ap *iwl_find_ap(const struct iwlwifi_state *st,
                                            const char *ssid);
static uint8_t *iwl_append_wpa2_psk_ccmp_rsn(uint8_t *p);

static const struct net_driver_ops iwlwifi_net_ops = {
    .transmit = iwlwifi_transmit,
    .poll = iwlwifi_poll,
};

static const struct iwl_device_info iwl_devices[] = {
    { 0x0082, "6205", "iwlwifi-6000g2a-18.168.6.1.fw" },
    { 0x0083, "1000", "iwlwifi-1000-39.31.5.1.fw" },
    { 0x0084, "1000", "iwlwifi-1000-39.31.5.1.fw" },
    { 0x0085, "6205", "iwlwifi-6000g2a-18.168.6.1.fw" },
    { 0x0087, "6050", "iwlwifi-6050-41.28.5.1.fw" },
    { 0x0089, "6050", "iwlwifi-6050-41.28.5.1.fw" },
    { 0x008a, "1030/6230", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x008b, "1030/6230", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x008c, "6000g2", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x0090, "1030/6230", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x0091, "1030/6230", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x0885, "6150", "iwlwifi-6050-41.28.5.1.fw" },
    { 0x0886, "6150", "iwlwifi-6050-41.28.5.1.fw" },
    { 0x0887, "2230", "iwlwifi-2030-18.168.6.1.fw" },
    { 0x0888, "2230", "iwlwifi-2030-18.168.6.1.fw" },
    { 0x088e, "6235", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x088f, "6235", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x0890, "2200", "iwlwifi-2000-18.168.6.1.fw" },
    { 0x0891, "2200", "iwlwifi-2000-18.168.6.1.fw" },
    { 0x0892, "135", "iwlwifi-135-6-18.168.6.1.fw" },
    { 0x0893, "135", "iwlwifi-135-6-18.168.6.1.fw" },
    { 0x0894, "105", "iwlwifi-105-6-18.168.6.1.fw" },
    { 0x0895, "105", "iwlwifi-105-6-18.168.6.1.fw" },
    { 0x0896, "130", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x0897, "130", "iwlwifi-6000g2b-18.168.6.1.fw" },
    { 0x08ae, "100", "iwlwifi-100-39.31.5.1.fw" },
    { 0x08af, "100", "iwlwifi-100-39.31.5.1.fw" },
    { 0x08b1, "7260", "iwlwifi-7260-17.ucode" },
    { 0x08b2, "7260", "iwlwifi-7260-17.ucode" },
    { 0x08b3, "3160", "iwlwifi-3160-17.ucode" },
    { 0x08b4, "3160", "iwlwifi-3160-17.ucode" },
    { 0x095a, "7265", "iwlwifi-7265-17.ucode" },
    { 0x095b, "7265", "iwlwifi-7265-17.ucode" },
    { 0x24f3, "8260", "iwlwifi-8000C-22.ucode" },
    { 0x24f4, "8260", "iwlwifi-8000C-22.ucode" },
    { 0x24f5, "8265", "iwlwifi-8265-22.ucode" },
    { 0x24fd, "8265", "iwlwifi-8265-22.ucode" },
    { 0x2526, "9260", "iwlwifi-9260-th-b0-jf-b0-34.ucode" },
    { 0x271b, "AX201", "iwlwifi-QuZ-a0-hr-b0-77.ucode" },
    { 0x2723, "AX200", "iwlwifi-cc-a0-77.ucode" },
    { 0x2725, "AX210", "iwlwifi-ty-a0-gf-a0-77.ucode" },
    { 0x2726, "AX201", "iwlwifi-QuZ-a0-hr-b0-77.ucode" },
    { 0x2729, "AX211", "iwlwifi-so-a0-gf-a0-77.ucode" },
    { 0x272b, "BE200", "iwlwifi-gl-c0-fm-c0-86.ucode" },
    { 0x4229, "4965", "iwlwifi-4965-228.61.2.24.fw" },
    { 0x422b, "6300", "iwlwifi-6000-9.221.4.1.fw" },
    { 0x422c, "6200", "iwlwifi-6000-9.221.4.1.fw" },
    { 0x422d, "4965", "iwlwifi-4965-228.61.2.24.fw" },
    { 0x4230, "4965", "iwlwifi-4965-228.61.2.24.fw" },
    { 0x4232, "5100", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x4233, "4965", "iwlwifi-4965-228.61.2.24.fw" },
    { 0x4235, "5300", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x4236, "5300", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x4237, "5100", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x4238, "6300", "iwlwifi-6000-9.221.4.1.fw" },
    { 0x4239, "6200", "iwlwifi-6000-9.221.4.1.fw" },
    { 0x423a, "5350", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x423b, "5350", "iwlwifi-5000-8.83.5.1.fw" },
    { 0x423c, "5150", "iwlwifi-5150-8.24.2.2.fw" },
    { 0x423d, "5150", "iwlwifi-5150-8.24.2.2.fw" },
    { 0x7af0, "AX201", "iwlwifi-QuZ-a0-hr-b0-77.ucode" },
    { 0x7e40, "AX211", "iwlwifi-so-a0-gf-a0-77.ucode" },
    { 0xa0f0, "AX201", "iwlwifi-QuZ-a0-hr-b0-77.ucode" },
    { 0x51f0, "AX211", "iwlwifi-so-a0-gf-a0-77.ucode" },
};

static const struct iwl_device_info *find_device(uint16_t id)
{
    for (size_t i = 0; i < sizeof(iwl_devices) / sizeof(iwl_devices[0]); i++) {
        if (iwl_devices[i].id == id) {
            return &iwl_devices[i];
        }
    }
    return NULL;
}

static bool iwl_device_uses_modern_transport(const struct iwl_device_info *info)
{
    if (!info || !info->family) {
        return false;
    }
    return strcmp(info->family, "3160") == 0 ||
           strcmp(info->family, "7260") == 0 ||
           strcmp(info->family, "7265") == 0 ||
           strcmp(info->family, "8260") == 0 ||
           strcmp(info->family, "8265") == 0 ||
           strcmp(info->family, "9260") == 0 ||
           strcmp(info->family, "AX200") == 0 ||
           strcmp(info->family, "AX201") == 0 ||
           strcmp(info->family, "AX210") == 0 ||
           strcmp(info->family, "AX211") == 0 ||
           strcmp(info->family, "BE200") == 0;
}

static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16_at(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t be16_at(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint64_t be64_at(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint64_t le64_at(const uint8_t *p)
{
    return (uint64_t)le32_at(p) | ((uint64_t)le32_at(p + 4) << 32);
}

static void iwl_mac_broadcast(uint8_t out[IEEE80211_ADDR_LEN])
{
    for (size_t i = 0; i < IEEE80211_ADDR_LEN; i++) {
        out[i] = 0xff;
    }
}

static bool iwl_mac_equal(const uint8_t a[IEEE80211_ADDR_LEN],
                          const uint8_t b[IEEE80211_ADDR_LEN])
{
    return memcmp(a, b, IEEE80211_ADDR_LEN) == 0;
}

static int iwl_memcmp_fixed(const uint8_t *a, const uint8_t *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

static uint16_t iwl_default_rxchain(void)
{
    uint16_t chains = 0x7;
    return IWL_RXCHAIN_DRIVER_FORCE |
           IWL_RXCHAIN_VALID(chains) |
           IWL_RXCHAIN_FORCE_SEL(chains) |
           IWL_RXCHAIN_FORCE_MIMO_SEL(chains) |
           IWL_RXCHAIN_IDLE_COUNT(1) |
           IWL_RXCHAIN_MIMO_COUNT(1);
}

static void iwl_cache_ap(struct iwlwifi_state *st, const char *ssid,
                         uint8_t ssid_len, const uint8_t bssid[IEEE80211_ADDR_LEN],
                         uint8_t channel, bool privacy, int8_t rssi)
{
    if (!ssid || ssid_len == 0 || ssid_len > 32 || !bssid) {
        return;
    }

    size_t slot = IWLWIFI_AP_CACHE_MAX;
    for (size_t i = 0; i < IWLWIFI_AP_CACHE_MAX; i++) {
        if (st->aps[i].valid && iwl_mac_equal(st->aps[i].bssid, bssid)) {
            slot = i;
            break;
        }
        if (!st->aps[i].valid && slot == IWLWIFI_AP_CACHE_MAX) {
            slot = i;
        }
    }
    if (slot == IWLWIFI_AP_CACHE_MAX) {
        return;
    }

    struct iwlwifi_ap *ap = &st->aps[slot];
    bool is_new = !ap->valid;
    memset(ap, 0, sizeof(*ap));
    ap->valid = true;
    ap->ssid_len = ssid_len;
    memcpy(ap->ssid, ssid, ssid_len);
    ap->ssid[ssid_len] = '\0';
    memcpy(ap->bssid, bssid, IEEE80211_ADDR_LEN);
    ap->channel = channel;
    ap->privacy = privacy;
    ap->rssi = rssi;
    if (is_new && st->ap_count < IWLWIFI_AP_CACHE_MAX) {
        st->ap_count++;
    }
    log_info("iwlwifi", "AP %s bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u privacy=%s",
             ap->ssid, ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5],
             ap->channel, ap->privacy ? "yes" : "no");
}

static void iwl_parse_mgmt_frame(struct iwlwifi_state *st, const uint8_t *frame,
                                 size_t len)
{
    if (!st || !frame || len < sizeof(struct ieee80211_hdr3) + 12) {
        return;
    }

    const struct ieee80211_hdr3 *hdr = (const struct ieee80211_hdr3 *)frame;
    if ((hdr->fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_MGT) {
        return;
    }
    uint8_t subtype = hdr->fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
    if (subtype == IEEE80211_FC0_SUBTYPE_AUTH && len >= sizeof(*hdr) + 6) {
        const uint8_t *body = frame + sizeof(*hdr);
        uint16_t alg = le16_at(body);
        uint16_t transaction = le16_at(body + 2);
        uint16_t status = le16_at(body + 4);
        if (alg == IEEE80211_AUTH_ALG_OPEN &&
            transaction == IEEE80211_AUTH_TRANSACTION_RESP) {
            st->auth_done = true;
            st->auth_status = status;
            log_info("iwlwifi", "auth response status=%u", status);
        }
        return;
    }
    if (subtype == IEEE80211_FC0_SUBTYPE_ASSOC_RESP && len >= sizeof(*hdr) + 6) {
        const uint8_t *body = frame + sizeof(*hdr);
        uint16_t status = le16_at(body + 2);
        uint16_t aid = le16_at(body + 4) & 0x3fff;
        st->assoc_done = true;
        st->assoc_status = status;
        st->assoc_id = aid;
        log_info("iwlwifi", "assoc response status=%u aid=%u", status, aid);
        return;
    }
    if (subtype != IEEE80211_FC0_SUBTYPE_BEACON &&
        subtype != IEEE80211_FC0_SUBTYPE_PROBE_RESP) {
        return;
    }

    const uint8_t *fixed = frame + sizeof(struct ieee80211_hdr3);
    uint16_t capability = le16_at(fixed + 10);
    const uint8_t *ies = fixed + 12;
    const uint8_t *end = frame + len;
    const uint8_t *ssid = NULL;
    uint8_t ssid_len = 0;
    uint8_t channel = 0;

    while (ies + 2 <= end) {
        uint8_t id = ies[0];
        uint8_t elen = ies[1];
        ies += 2;
        if (ies + elen > end) {
            break;
        }
        if (id == IEEE80211_ELEMID_SSID && elen <= 32) {
            ssid = ies;
            ssid_len = elen;
        } else if (id == IEEE80211_ELEMID_DSPARMS && elen >= 1) {
            channel = ies[0];
        }
        ies += elen;
    }

    if (ssid && ssid_len) {
        char ssid_text[33];
        memcpy(ssid_text, ssid, ssid_len);
        ssid_text[ssid_len] = '\0';
        iwl_cache_ap(st, ssid_text, ssid_len, hdr->addr3, channel,
                     (capability & IEEE80211_CAPINFO_PRIVACY) != 0, 0);
    }
}

static void iwl_make_snonce(struct iwlwifi_state *st, const struct net_iface *iface)
{
    uint8_t seed[64];
    memset(seed, 0, sizeof(seed));
    memcpy(seed, iface->mac, 6);
    memcpy(seed + 6, st->bssid, 6);
    uint64_t ticks = pit_ticks();
    for (size_t i = 0; i < 8; i++) {
        seed[16 + i] = (uint8_t)(ticks >> (i * 8));
    }
    tnu_sha1(seed, sizeof(seed), st->wpa_snonce);
    seed[31] ^= 0xa5;
    uint8_t tail[TNU_SHA1_DIGEST_SIZE];
    tnu_sha1(seed, sizeof(seed), tail);
    memcpy(st->wpa_snonce + 20, tail, 12);
}

static void iwl_derive_ptk(struct iwlwifi_state *st, const struct net_iface *iface)
{
    uint8_t data[76];
    const uint8_t *mac1 = iface->mac;
    const uint8_t *mac2 = st->bssid;
    if (iwl_memcmp_fixed(mac1, mac2, 6) > 0) {
        const uint8_t *tmp = mac1;
        mac1 = mac2;
        mac2 = tmp;
    }
    memcpy(data, mac1, 6);
    memcpy(data + 6, mac2, 6);

    const uint8_t *nonce1 = st->wpa_snonce;
    const uint8_t *nonce2 = st->wpa_anonce;
    if (iwl_memcmp_fixed(nonce1, nonce2, 32) > 0) {
        const uint8_t *tmp = nonce1;
        nonce1 = nonce2;
        nonce2 = tmp;
    }
    memcpy(data + 12, nonce1, 32);
    memcpy(data + 44, nonce2, 32);
    tnu_wpa_prf(st->wpa_pmk, TNU_WPA_PMK_LEN, "Pairwise key expansion",
                data, sizeof(data), st->wpa_ptk, TNU_WPA_PTK_LEN);
    st->wpa_ptk_ready = true;
}

static size_t iwl_build_eapol_key_msg2(struct iwlwifi_state *st, const struct iwlwifi_ap *ap,
                                       uint8_t *out, size_t out_size)
{
    uint8_t rsn[22];
    uint8_t *p = rsn;
    p = iwl_append_wpa2_psk_ccmp_rsn(p);
    size_t rsn_len = (size_t)(p - rsn);
    size_t eapol_len = 4 + WPA_EAPOL_KEY_BASE_LEN + rsn_len;
    if (!st->wpa_ptk_ready || out_size < eapol_len) {
        return 0;
    }

    memset(out, 0, eapol_len);
    out[0] = EAPOL_VERSION_2;
    out[1] = EAPOL_TYPE_KEY;
    put_be16(out + 2, (uint16_t)(WPA_EAPOL_KEY_BASE_LEN + rsn_len));
    uint8_t *key = out + 4;
    key[0] = RSN_KEY_DESC;
    uint16_t info = WPA_KEY_INFO_TYPE_HMAC_SHA1_AES |
                    WPA_KEY_INFO_KEY_TYPE |
                    WPA_KEY_INFO_MIC;
    put_be16(key + 1, info);
    put_be16(key + 3, 16);
    put_be64(key + 5, st->wpa_replay_counter);
    memcpy(key + 13, st->wpa_snonce, 32);
    put_be16(key + 93, (uint16_t)rsn_len);
    memcpy(key + 95, rsn, rsn_len);

    uint8_t mic[TNU_SHA1_DIGEST_SIZE];
    tnu_hmac_sha1(st->wpa_ptk, 16, out, eapol_len, mic);
    memcpy(key + 77, mic, 16);
    (void)ap;
    return eapol_len;
}

static size_t iwl_build_eapol_key_msg4(struct iwlwifi_state *st,
                                       uint8_t *out, size_t out_size)
{
    size_t eapol_len = 4 + WPA_EAPOL_KEY_BASE_LEN;
    if (!st->wpa_ptk_ready || out_size < eapol_len) {
        return 0;
    }
    memset(out, 0, eapol_len);
    out[0] = EAPOL_VERSION_2;
    out[1] = EAPOL_TYPE_KEY;
    put_be16(out + 2, WPA_EAPOL_KEY_BASE_LEN);
    uint8_t *key = out + 4;
    key[0] = RSN_KEY_DESC;
    uint16_t info = WPA_KEY_INFO_TYPE_HMAC_SHA1_AES |
                    WPA_KEY_INFO_KEY_TYPE |
                    WPA_KEY_INFO_MIC |
                    WPA_KEY_INFO_SECURE;
    put_be16(key + 1, info);
    put_be16(key + 3, 16);
    put_be64(key + 5, st->wpa_replay_counter);
    uint8_t mic[TNU_SHA1_DIGEST_SIZE];
    tnu_hmac_sha1(st->wpa_ptk, 16, out, eapol_len, mic);
    memcpy(key + 77, mic, 16);
    return eapol_len;
}

static bool iwl_verify_eapol_mic(struct iwlwifi_state *st,
                                 const uint8_t *eapol, size_t eapol_len)
{
    if (!st->wpa_ptk_ready || !eapol || eapol_len > 512 ||
        eapol_len < 4 + WPA_EAPOL_KEY_BASE_LEN) {
        return false;
    }
    uint8_t tmp[512];
    memcpy(tmp, eapol, eapol_len);
    memset(tmp + 4 + 77, 0, 16);
    uint8_t mic[TNU_SHA1_DIGEST_SIZE];
    tnu_hmac_sha1(st->wpa_ptk, 16, tmp, eapol_len, mic);
    return memcmp(mic, eapol + 4 + 77, 16) == 0;
}

static int iwl_send_eapol_frame(struct iwlwifi_state *st, const struct net_iface *iface,
                                const struct iwlwifi_ap *ap,
                                const uint8_t *eapol, size_t eapol_len)
{
    uint8_t frame[768];
    if (!st || !iface || !ap || !eapol ||
        sizeof(struct ieee80211_hdr3) + 8 + eapol_len > sizeof(frame)) {
        return -1;
    }
    struct ieee80211_hdr3 *hdr = (struct ieee80211_hdr3 *)frame;
    memset(hdr, 0, sizeof(*hdr));
    hdr->fc[0] = IEEE80211_FC0_TYPE_DATA;
    hdr->fc[1] = IEEE80211_FC1_DIR_TODS;
    memcpy(hdr->addr1, ap->bssid, 6);
    memcpy(hdr->addr2, iface->mac, 6);
    memcpy(hdr->addr3, ap->bssid, 6);
    put_le16(hdr->seq, (uint16_t)(st->tx_sequence++ << 4));

    uint8_t *p = frame + sizeof(*hdr);
    static const uint8_t llc_snap_eapol[] = {
        0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e
    };
    memcpy(p, llc_snap_eapol, sizeof(llc_snap_eapol));
    p += sizeof(llc_snap_eapol);
    memcpy(p, eapol, eapol_len);
    p += eapol_len;
    return iwl_send_mgmt_frame(st, frame, (size_t)(p - frame), true);
}

static int iwl_handle_eapol_key(struct iwlwifi_state *st, const struct net_iface *iface,
                                const struct iwlwifi_ap *ap,
                                const uint8_t *eapol, size_t eapol_len)
{
    if (!st || !iface || !ap || !eapol || eapol_len < 4 + WPA_EAPOL_KEY_BASE_LEN ||
        eapol[1] != EAPOL_TYPE_KEY) {
        return -1;
    }
    const uint8_t *key = eapol + 4;
    if (key[0] != RSN_KEY_DESC) {
        return -1;
    }
    uint16_t info = be16_at(key + 1);
    uint64_t replay = be64_at(key + 5);
    bool ack = (info & WPA_KEY_INFO_ACK) != 0;
    bool mic = (info & WPA_KEY_INFO_MIC) != 0;
    bool install = (info & WPA_KEY_INFO_INSTALL) != 0;

    if (ack && !mic) {
        memcpy(st->wpa_anonce, key + 13, 32);
        st->wpa_replay_counter = replay;
        if (!st->wpa_snonce[0]) {
            iwl_make_snonce(st, iface);
        }
        iwl_derive_ptk(st, iface);
        uint8_t response[256];
        size_t response_len = iwl_build_eapol_key_msg2(st, ap, response, sizeof(response));
        if (response_len == 0 ||
            iwl_send_eapol_frame(st, iface, ap, response, response_len) < 0) {
            log_warn("iwlwifi", "%s could not transmit WPA msg2", iface->name);
            return -2;
        }
        st->wpa_key_msg1 = true;
        log_info("iwlwifi", "%s processed WPA msg1 and transmitted msg2", iface->name);
        return 0;
    }

    if (ack && mic && install) {
        if (!iwl_verify_eapol_mic(st, eapol, eapol_len)) {
            log_warn("iwlwifi", "%s WPA msg3 MIC verification failed", iface->name);
            return -3;
        }
        st->wpa_key_msg3 = true;
        st->wpa_replay_counter = replay;
        uint8_t response[128];
        size_t response_len = iwl_build_eapol_key_msg4(st, response, sizeof(response));
        if (response_len == 0 ||
            iwl_send_eapol_frame(st, iface, ap, response, response_len) < 0) {
            log_warn("iwlwifi", "%s could not transmit WPA msg4", iface->name);
            return -4;
        }
        log_warn("iwlwifi", "%s verified WPA msg3 and transmitted msg4; GTK unwrap is next",
                 iface->name);
        return 0;
    }
    return -1;
}

static void iwl_parse_data_frame(struct iwlwifi_state *st, const struct net_iface *iface,
                                 const uint8_t *frame, size_t len)
{
    if (!st || !iface || len < sizeof(struct ieee80211_hdr3) + 8) {
        return;
    }
    const struct ieee80211_hdr3 *hdr = (const struct ieee80211_hdr3 *)frame;
    if ((hdr->fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA ||
        (hdr->fc[1] & IEEE80211_FC1_PROTECTED)) {
        return;
    }
    size_t hdr_len = sizeof(struct ieee80211_hdr3);
    if ((hdr->fc[0] & IEEE80211_FC0_SUBTYPE_MASK) & 0x80) {
        hdr_len += 2;
    }
    if (len < hdr_len + 8) {
        return;
    }
    const uint8_t *llc = frame + hdr_len;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03 ||
        (llc[3] | llc[4] | llc[5]) != 0) {
        return;
    }
    uint16_t ethertype = be16_at(llc + 6);
    if (ethertype == ETH_TYPE_EAPOL) {
        const struct iwlwifi_ap *ap = iwl_find_ap(st, st->associated_ssid);
        if (!ap) {
            return;
        }
        iwl_handle_eapol_key(st, iface, ap, llc + 8, len - hdr_len - 8);
        return;
    }
    if (!st->link_ready || !st->rx_callback) {
        return;
    }
    uint8_t eth[1600];
    size_t payload_len = len - hdr_len - 8;
    if (14 + payload_len > sizeof(eth)) {
        return;
    }
    memcpy(eth, hdr->addr1, 6);
    memcpy(eth + 6, hdr->addr3, 6);
    eth[12] = (uint8_t)(ethertype >> 8);
    eth[13] = (uint8_t)ethertype;
    memcpy(eth + 14, llc + 8, payload_len);
    st->rx_callback(st->iface, eth, 14 + payload_len, st->rx_callback_ctx);
}

static void iwl_handle_mpdu(struct iwlwifi_state *st, const uint8_t *payload,
                            size_t payload_len)
{
    if (!st || !payload || payload_len < sizeof(struct iwl_rx_mpdu) + 4) {
        return;
    }
    const struct iwl_rx_mpdu *mpdu = (const struct iwl_rx_mpdu *)payload;
    size_t frame_len = mpdu->len;
    if (frame_len == 0 || payload_len < sizeof(*mpdu) + frame_len + 4) {
        return;
    }
    const uint8_t *frame = payload + sizeof(*mpdu);
    uint32_t status = le32_at(frame + frame_len);
    if ((status & IWL_RX_NOERROR) != IWL_RX_NOERROR) {
        return;
    }
    if ((frame[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA) {
        iwl_parse_data_frame(st, st->iface, frame, frame_len);
    } else {
        iwl_parse_mgmt_frame(st, frame, frame_len);
    }
}

static bool part_present(const struct iwlwifi_fw_part *part)
{
    return part->text && part->text_size && part->data && part->data_size;
}

static uint8_t firmware_api(uint32_t version)
{
    return (uint8_t)((version >> 8) & 0xff);
}

static bool iwl_validate_tlv_firmware(struct iwlwifi_state *st)
{
    const uint8_t *fw = st->firmware_data;
    size_t size = st->firmware_size;
    if (size < IWL_TLV_HEADER_SIZE || le32_at(fw) != 0 ||
        le32_at(fw + 4) != IWL_TLV_UCODE_MAGIC) {
        return false;
    }

    memcpy(st->firmware_human, fw + 8, IWL_TLV_HUMAN_SIZE);
    st->firmware_human[IWL_TLV_HUMAN_SIZE] = '\0';
    st->firmware_version = le32_at(fw + 72);
    st->firmware_build = le32_at(fw + 76);
    st->firmware_tlv = true;
    return true;
}

static bool iwl_parse_tlv_sections(struct iwlwifi_state *st)
{
    const uint8_t *ptr = st->firmware_data + IWL_TLV_HEADER_SIZE;
    const uint8_t *end = st->firmware_data + st->firmware_size;
    uint64_t altmask = le64_at(st->firmware_data + 80);
    uint16_t alt = 0;

    for (uint16_t candidate = 3; candidate > 0; candidate--) {
        if (altmask & (1ull << candidate)) {
            alt = candidate;
            break;
        }
    }

    while (ptr + 8 <= end) {
        uint16_t type = le16_at(ptr);
        uint16_t entry_alt = le16_at(ptr + 2);
        uint32_t len = le32_at(ptr + 4);
        ptr += 8;
        if (ptr + len > end) {
            log_warn("iwlwifi", "firmware %s TLV section overruns image",
                     st->firmware_name);
            return false;
        }
        if (entry_alt == 0 || entry_alt == alt) {
            switch (type) {
            case IWL_TLV_MAIN_TEXT:
                st->runtime_ucode.text = ptr;
                st->runtime_ucode.text_size = len;
                break;
            case IWL_TLV_MAIN_DATA:
                st->runtime_ucode.data = ptr;
                st->runtime_ucode.data_size = len;
                break;
            case IWL_TLV_INIT_TEXT:
                st->init_ucode.text = ptr;
                st->init_ucode.text_size = len;
                break;
            case IWL_TLV_INIT_DATA:
                st->init_ucode.data = ptr;
                st->init_ucode.data_size = len;
                break;
            case IWL_TLV_BOOT_TEXT:
                st->boot_ucode.text = ptr;
                st->boot_ucode.text_size = len;
                break;
            default:
                break;
            }
        }
        ptr += (len + 3u) & ~3u;
    }

    st->firmware_parsed = part_present(&st->runtime_ucode) &&
                          part_present(&st->init_ucode);
    return st->firmware_parsed;
}

static bool iwl_validate_legacy_firmware(struct iwlwifi_state *st)
{
    const uint8_t *fw = st->firmware_data;
    size_t size = st->firmware_size;
    if (size < 28) {
        return false;
    }

    uint32_t version = le32_at(fw);
    size_t header = firmware_api(version) >= 3 ? 28 : 24;
    if (size < header) {
        return false;
    }
    const uint8_t *sizes = fw + (header == 28 ? 8 : 4);
    uint32_t main_text = le32_at(sizes);
    uint32_t main_data = le32_at(sizes + 4);
    uint32_t init_text = le32_at(sizes + 8);
    uint32_t init_data = le32_at(sizes + 12);
    uint32_t boot_text = le32_at(sizes + 16);
    uint64_t total = (uint64_t)header + main_text + main_data +
                     init_text + init_data + boot_text;

    if (total > size) {
        return false;
    }

    st->firmware_version = version;
    st->firmware_build = 0;
    st->firmware_tlv = false;
    ksnprintf(st->firmware_human, sizeof(st->firmware_human),
              "legacy iwl firmware v%u", version);
    return true;
}

static bool iwl_parse_legacy_sections(struct iwlwifi_state *st)
{
    const uint8_t *fw = st->firmware_data;
    size_t header = firmware_api(st->firmware_version) >= 3 ? 28 : 24;
    const uint8_t *sizes = fw + (header == 28 ? 8 : 4);
    uint32_t main_text = le32_at(sizes);
    uint32_t main_data = le32_at(sizes + 4);
    uint32_t init_text = le32_at(sizes + 8);
    uint32_t init_data = le32_at(sizes + 12);
    uint32_t boot_text = le32_at(sizes + 16);
    const uint8_t *ptr = fw + header;

    st->runtime_ucode.text = ptr;
    st->runtime_ucode.text_size = main_text;
    ptr += main_text;
    st->runtime_ucode.data = ptr;
    st->runtime_ucode.data_size = main_data;
    ptr += main_data;
    st->init_ucode.text = ptr;
    st->init_ucode.text_size = init_text;
    ptr += init_text;
    st->init_ucode.data = ptr;
    st->init_ucode.data_size = init_data;
    ptr += init_data;
    st->boot_ucode.text = ptr;
    st->boot_ucode.text_size = boot_text;

    st->firmware_parsed = part_present(&st->runtime_ucode) &&
                          part_present(&st->init_ucode);
    return st->firmware_parsed;
}

static bool iwl_parse_firmware_sections(struct iwlwifi_state *st)
{
    bool ok = st->firmware_tlv ? iwl_parse_tlv_sections(st)
                               : iwl_parse_legacy_sections(st);
    if (!ok) {
        log_warn("iwlwifi", "firmware %s lacks required init/runtime text/data sections",
                 st->firmware_name);
        return false;
    }
    log_info("iwlwifi", "sections runtime text=%u data=%u init text=%u data=%u boot=%u",
             st->runtime_ucode.text_size, st->runtime_ucode.data_size,
             st->init_ucode.text_size, st->init_ucode.data_size,
             st->boot_ucode.text_size);
    return true;
}

static int iwl_alloc_firmware_dma(struct iwlwifi_state *st)
{
    if (st->firmware_dma) {
        return 0;
    }

    size_t pages = (IWL_FW_DMA_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t first = 0;
    uintptr_t prev = 0;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t frame = pmm_alloc_frame();
        if (!frame) {
            return -1;
        }
        if (i == 0) {
            first = frame;
        } else if (frame != prev + PAGE_SIZE) {
            return -1;
        }
        prev = frame;
    }
    if (vmm_map_range_identity(first, pages * PAGE_SIZE, 0) < 0) {
        return -1;
    }
    st->firmware_dma = (void *)first;
    st->firmware_dma_phys = first;
    st->firmware_dma_size = pages * PAGE_SIZE;
    return 0;
}

static int iwl_stage_firmware_dma(struct iwlwifi_state *st,
                                  const struct iwlwifi_fw_part *part)
{
    size_t total = (size_t)part->text_size + part->data_size;
    if (!part_present(part) || total > IWL_FW_DMA_SIZE) {
        return -1;
    }
    if (iwl_alloc_firmware_dma(st) < 0) {
        return -2;
    }
    memcpy(st->firmware_dma, part->text, part->text_size);
    memcpy((uint8_t *)st->firmware_dma + part->text_size,
           part->data, part->data_size);
    st->firmware_staged = true;
    log_info("iwlwifi", "staged runtime firmware in DMA buffer %p phys=%p (%llu bytes)",
             st->firmware_dma, (void *)st->firmware_dma_phys,
             (unsigned long long)total);
    return 0;
}

static int iwl_alloc_contiguous_dma(size_t size, uintptr_t *phys, void **virt)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uintptr_t first = 0;
    uintptr_t prev = 0;
    for (size_t i = 0; i < pages; i++) {
        uintptr_t frame = pmm_alloc_frame();
        if (!frame) {
            return -1;
        }
        if (i == 0) {
            first = frame;
        } else if (frame != prev + PAGE_SIZE) {
            return -2;
        }
        prev = frame;
    }
    if (vmm_map_range_identity(first, pages * PAGE_SIZE, 0) < 0) {
        return -3;
    }
    *phys = first;
    *virt = (void *)first;
    memset(*virt, 0, pages * PAGE_SIZE);
    return 0;
}

static int iwl_alloc_rx_dma(struct iwlwifi_state *st)
{
    if (st->rx_desc) {
        return 0;
    }
    uintptr_t phys;
    void *virt;
    int rc = iwl_alloc_contiguous_dma(IWL_RX_DMA_SIZE, &phys, &virt);
    if (rc < 0) {
        return rc;
    }
    st->rx_desc = virt;
    st->rx_desc_phys = phys;
    st->rx_status = (uint8_t *)virt + PAGE_SIZE;
    st->rx_status_phys = phys + PAGE_SIZE;
    st->rx_buffers = (uint8_t *)virt + PAGE_SIZE * 2;
    st->rx_buffers_phys = phys + PAGE_SIZE * 2;
    st->rx_dma_size = IWL_RX_DMA_SIZE;
    st->rx_index = 0;

    for (size_t i = 0; i < IWL_RX_RING_COUNT; i++) {
        st->rx_desc[i] = (uint32_t)((st->rx_buffers_phys +
                                     i * IWL_RX_BUF_SIZE) >> 8);
    }
    log_info("iwlwifi", "allocated RX DMA desc=%p status=%p buffers=%p",
             (void *)st->rx_desc_phys, (void *)st->rx_status_phys,
             (void *)st->rx_buffers_phys);
    return 0;
}

static int iwl_init_rx_ring(struct iwlwifi_state *st)
{
    if (iwl_alloc_rx_dma(st) < 0) {
        return -1;
    }
    if (iwl_nic_lock(st) < 0) {
        return -2;
    }
    iwl_write32(st, FH_RX_CONFIG, 0);
    iwl_write32(st, FH_RX_WPTR, 0);
    iwl_write32(st, FH_RX_BASE, (uint32_t)(st->rx_desc_phys >> 8));
    iwl_write32(st, FH_STATUS_WPTR, (uint32_t)(st->rx_status_phys >> 4));
    iwl_write32(st, FH_RX_CONFIG,
                IWL_FH_RX_CONFIG_ENA |
                IWL_FH_RX_CONFIG_IGNORE_EMPTY |
                IWL_FH_RX_CONFIG_IRQ_HOST |
                IWL_FH_RX_CONFIG_SINGLE |
                IWL_FH_RX_CONFIG_TIMEOUT(0) |
                IWL_FH_RX_CONFIG_NRBD(IWL_RX_RING_LOG));
    iwl_nic_unlock(st);
    iwl_write32(st, FH_RX_WPTR, (IWL_RX_RING_COUNT - 1) & ~7u);
    return 0;
}

static int iwl_init_cmd_queue(struct iwlwifi_state *st)
{
    if (st->cmd_queue_ready) {
        return 0;
    }
    uintptr_t phys;
    void *virt;
    int rc = iwl_alloc_contiguous_dma(IWL_CMD_DMA_SIZE, &phys, &virt);
    if (rc < 0) {
        return rc;
    }

    st->cmd_desc = virt;
    st->cmd_desc_phys = phys;
    st->cmd_ring = (uint8_t *)virt + IWL_TX_RING_COUNT * IWL_TX_DESC_SIZE;
    st->cmd_ring_phys = phys + IWL_TX_RING_COUNT * IWL_TX_DESC_SIZE;
    st->cmd_dma_size = IWL_CMD_DMA_SIZE;
    st->cmd_index = 0;

    if (iwl_nic_lock(st) < 0) {
        return -2;
    }
    iwl_write32(st, FH_CBBC_QUEUE(IWL_CMD_QUEUE), (uint32_t)(st->cmd_desc_phys >> 8));
    iwl_write32(st, FH_TX_CONFIG(IWL_CMD_QUEUE),
                IWL_FH_TX_CONFIG_DMA_ENA | IWL_FH_TX_CONFIG_DMA_CREDIT_ENA);
    iwl_nic_unlock(st);

    st->cmd_queue_ready = true;
    log_info("iwlwifi", "allocated command queue desc=%p ring=%p",
             (void *)st->cmd_desc_phys, (void *)st->cmd_ring_phys);
    return 0;
}

static int iwl_init_tx_queue(struct iwlwifi_state *st)
{
    if (st->tx_queue_ready) {
        return 0;
    }
    uintptr_t phys;
    void *virt;
    int rc = iwl_alloc_contiguous_dma(IWL_CMD_DMA_SIZE, &phys, &virt);
    if (rc < 0) {
        return rc;
    }

    st->tx_desc = virt;
    st->tx_desc_phys = phys;
    st->tx_ring = (uint8_t *)virt + IWL_TX_RING_COUNT * IWL_TX_DESC_SIZE;
    st->tx_ring_phys = phys + IWL_TX_RING_COUNT * IWL_TX_DESC_SIZE;
    st->tx_dma_size = IWL_CMD_DMA_SIZE;
    st->tx_index = 0;

    if (iwl_nic_lock(st) < 0) {
        return -2;
    }
    iwl_write32(st, FH_CBBC_QUEUE(IWL_MGMT_QUEUE), (uint32_t)(st->tx_desc_phys >> 8));
    iwl_write32(st, FH_TX_CONFIG(IWL_MGMT_QUEUE),
                IWL_FH_TX_CONFIG_DMA_ENA | IWL_FH_TX_CONFIG_DMA_CREDIT_ENA);
    iwl_nic_unlock(st);

    st->tx_queue_ready = true;
    log_info("iwlwifi", "allocated management TX queue desc=%p ring=%p",
             (void *)st->tx_desc_phys, (void *)st->tx_ring_phys);
    return 0;
}

static int iwl_load_firmware(struct iwlwifi_state *st)
{
    char path[VFS_PATH_MAX];
    struct vfs_node *node = NULL;

    ksnprintf(path, sizeof(path), "/lib/firmware/iwlwifi/%s", st->firmware_name);
    node = vfs_lookup(path, "/");
    if (!node) {
        ksnprintf(path, sizeof(path), "/lib/firmware/%s", st->firmware_name);
        node = vfs_lookup(path, "/");
    }
    if (!node || node->type != VFS_NODE_FILE || !node->data || node->size == 0) {
        log_warn("iwlwifi", "firmware %s not found under /lib/firmware/iwlwifi",
                 st->firmware_name);
        return -1;
    }

    st->firmware_data = node->data;
    st->firmware_size = (size_t)node->size;
    if (!iwl_validate_tlv_firmware(st) && !iwl_validate_legacy_firmware(st)) {
        log_warn("iwlwifi", "firmware %s is present but has an invalid header (%llu bytes)",
                 st->firmware_name, (unsigned long long)node->size);
        st->firmware_data = NULL;
        st->firmware_size = 0;
        return -2;
    }

    st->firmware_loaded = true;
    log_info("iwlwifi", "loaded %s (%llu KiB, %s, version=%u build=%u)",
             st->firmware_name, (unsigned long long)(st->firmware_size / 1024),
             st->firmware_tlv ? "TLV" : "legacy",
             st->firmware_version, st->firmware_build);
    if (st->firmware_human[0]) {
        log_info("iwlwifi", "firmware says: %s", st->firmware_human);
    }
    if (iwl_parse_firmware_sections(st)) {
        int rc = iwl_stage_firmware_dma(st, &st->runtime_ucode);
        if (rc < 0) {
            log_warn("iwlwifi", "could not stage firmware DMA buffer (%d)", rc);
        }
    }
    return 0;
}

static uint32_t iwl_read32(const struct iwlwifi_state *st, uint32_t reg)
{
    return *(volatile uint32_t *)(st->mmio + reg);
}

static void iwl_write32(const struct iwlwifi_state *st, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(st->mmio + reg) = value;
}

static int iwl_nic_lock(const struct iwlwifi_state *st)
{
    iwl_write32(st, CSR_GP_CNTRL,
                iwl_read32(st, CSR_GP_CNTRL) | IWL_GP_CNTRL_MAC_ACCESS_REQ);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = iwl_read32(st, CSR_GP_CNTRL);
        if ((v & (IWL_GP_CNTRL_MAC_ACCESS_ENA | IWL_GP_CNTRL_SLEEP)) ==
            IWL_GP_CNTRL_MAC_ACCESS_ENA) {
            return 0;
        }
    }
    return -1;
}

static void iwl_nic_unlock(const struct iwlwifi_state *st)
{
    iwl_write32(st, CSR_GP_CNTRL,
                iwl_read32(st, CSR_GP_CNTRL) & ~IWL_GP_CNTRL_MAC_ACCESS_REQ);
}

static uint32_t iwl_prph_read(const struct iwlwifi_state *st, uint32_t addr)
{
    iwl_write32(st, CSR_PRPH_RADDR, IWL_PRPH_DWORD | addr);
    return iwl_read32(st, CSR_PRPH_RDATA);
}

static void iwl_prph_write(const struct iwlwifi_state *st, uint32_t addr, uint32_t value)
{
    iwl_write32(st, CSR_PRPH_WADDR, IWL_PRPH_DWORD | addr);
    iwl_write32(st, CSR_PRPH_WDATA, value);
}

static void iwl_prph_setbits(const struct iwlwifi_state *st, uint32_t addr, uint32_t bits)
{
    iwl_prph_write(st, addr, iwl_prph_read(st, addr) | bits);
}

static void iwl_prph_clrbits(const struct iwlwifi_state *st, uint32_t addr, uint32_t bits)
{
    iwl_prph_write(st, addr, iwl_prph_read(st, addr) & ~bits);
}

static void iwl_setbits(const struct iwlwifi_state *st, uint32_t reg, uint32_t bits)
{
    iwl_write32(st, reg, iwl_read32(st, reg) | bits);
}

static void iwl_clrbits(const struct iwlwifi_state *st, uint32_t reg, uint32_t bits)
{
    iwl_write32(st, reg, iwl_read32(st, reg) & ~bits);
}

static bool iwl_wait_bit_set(const struct iwlwifi_state *st, uint32_t reg,
                             uint32_t mask, uint64_t timeout_ticks)
{
    uint64_t start = pit_ticks();
    do {
        if ((iwl_read32(st, reg) & mask) == mask) {
            return true;
        }
    } while (pit_ticks() - start <= timeout_ticks);
    return false;
}

static bool iwl_wait_bit_clear(const struct iwlwifi_state *st, uint32_t reg,
                               uint32_t mask, uint64_t timeout_ticks)
{
    uint64_t start = pit_ticks();
    do {
        if ((iwl_read32(st, reg) & mask) == 0) {
            return true;
        }
    } while (pit_ticks() - start <= timeout_ticks);
    return false;
}

static void iwl_short_delay(void)
{
    for (volatile int i = 0; i < 20000; i++) {
    }
}

static int iwl_hw_prepare(struct iwlwifi_state *st)
{
    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_NIC_READY);
    if (iwl_wait_bit_set(st, CSR_HW_IF_CONFIG_REG,
                         IWL_HW_IF_CONFIG_NIC_READY, 2)) {
        return 0;
    }

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_PREPARE);
    if (!iwl_wait_bit_clear(st, CSR_HW_IF_CONFIG_REG,
                            IWL_HW_IF_CONFIG_PREPARE_DONE, 150)) {
        return -1;
    }

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_NIC_READY);
    if (!iwl_wait_bit_set(st, CSR_HW_IF_CONFIG_REG,
                          IWL_HW_IF_CONFIG_NIC_READY, 2)) {
        return -2;
    }
    return 0;
}

static int iwl_clock_wait(struct iwlwifi_state *st)
{
    if (iwl_wait_bit_set(st, CSR_GP_CNTRL,
                         IWL_GP_CNTRL_MAC_CLOCK_READY, 25)) {
        return 0;
    }
    return -1;
}

static int iwl_apm_init(struct iwlwifi_state *st)
{
    int rc = iwl_hw_prepare(st);
    if (rc < 0) {
        log_warn("iwlwifi", "hardware prepare failed (%d)", rc);
        return rc;
    }

    iwl_setbits(st, CSR_GIO_CHICKEN,
                IWL_GIO_CHICKEN_DIS_L0S_TIMER | IWL_GIO_CHICKEN_L1A_NO_L0S_RX);
    iwl_setbits(st, CSR_DBG_HPET_MEM, 0xffff0000u);
    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_HAP_WAKE_L1A);
    iwl_setbits(st, CSR_RESET, IWL_RESET_LINK_PWR_MGMT_DIS);
    iwl_clrbits(st, CSR_GIO, IWL_GIO_L0S_ENA);

    rc = iwl_clock_wait(st);
    if (rc < 0) {
        log_warn("iwlwifi", "MAC clock did not become ready");
        return rc;
    }

    rc = iwl_nic_lock(st);
    if (rc < 0) {
        log_warn("iwlwifi", "could not lock NIC during APM init");
        return rc;
    }
    uint32_t clk = IWL_APMG_CLK_CTRL_DMA_CLK_RQT;
    if (strcmp(st->family, "4965") == 0) {
        clk |= IWL_APMG_CLK_CTRL_BSM_CLK_RQT;
    }
    iwl_prph_write(st, IWL_APMG_CLK_EN, clk);
    iwl_short_delay();
    iwl_prph_setbits(st, IWL_APMG_PCI_STT, IWL_APMG_PCI_STT_L1A_DIS);
    iwl_prph_clrbits(st, IWL_APMG_PS, IWL_APMG_PS_PWR_SRC_MASK);
    iwl_prph_setbits(st, IWL_APMG_PS, IWL_APMG_PS_EARLY_PWROFF_DIS);
    if (strcmp(st->family, "1000") == 0) {
        uint32_t svr = iwl_prph_read(st, IWL_APMG_DIGITAL_SVR);
        svr &= ~IWL_APMG_DIGITAL_SVR_VOLTAGE_MASK;
        svr |= IWL_APMG_DIGITAL_SVR_VOLTAGE_1_32;
        iwl_prph_write(st, IWL_APMG_DIGITAL_SVR, svr);
    }
    iwl_nic_unlock(st);

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG,
                IWL_HW_IF_CONFIG_RADIO_SI | IWL_HW_IF_CONFIG_MAC_SI);
    iwl_write32(st, CSR_UCODE_GP1_CLR, IWL_UCODE_GP1_RFKILL);
    iwl_write32(st, CSR_UCODE_GP1_CLR, IWL_UCODE_GP1_CMD_BLOCKED);
    return 0;
}

static bool iwl_wait_int(struct iwlwifi_state *st, uint32_t want, uint32_t *seen,
                         uint64_t timeout_ticks)
{
    uint64_t start = pit_ticks();
    while (pit_ticks() - start <= timeout_ticks) {
        uint32_t v = iwl_read32(st, CSR_INT);
        uint32_t fh = iwl_read32(st, CSR_FH_INT_STATUS);
        if (seen) {
            *seen = v | fh;
        }
        if (v & (IWL_INT_SW_ERR | IWL_INT_HW_ERR)) {
            iwl_write32(st, CSR_INT, v);
            iwl_write32(st, CSR_FH_INT_STATUS, fh);
            return false;
        }
        if ((v & want) || (fh & want)) {
            iwl_write32(st, CSR_INT, v);
            iwl_write32(st, CSR_FH_INT_STATUS, fh);
            return true;
        }
    }
    return false;
}

static void iwl_poll_rx_notifications(struct iwlwifi_state *st)
{
    if (!st->rx_status || !st->rx_buffers) {
        return;
    }
    volatile struct iwl_rx_status *status = st->rx_status;
    uint16_t hw = status->closed_count & (IWL_RX_RING_COUNT - 1);

    while (st->rx_index != hw) {
        uint8_t *buf = st->rx_buffers + st->rx_index * IWL_RX_BUF_SIZE;
        struct iwl_rx_desc *desc = (struct iwl_rx_desc *)buf;
        uint32_t len = desc->len & 0x3fffu;
        if (len < sizeof(*desc) || len > IWL_RX_BUF_SIZE) {
            break;
        }

        uint8_t code = desc->type;
        bool unsolicited = (desc->qid & IWL_RX_UNSOLICITED) != 0;
        if (!unsolicited) {
            st->command_done = true;
            st->command_done_code = code;
        }

        switch (code) {
        case IWL_RX_TYPE_UC_READY:
            if (len >= sizeof(*desc) + sizeof(struct iwl_ucode_info)) {
                const struct iwl_ucode_info *uc =
                    (const struct iwl_ucode_info *)(buf + sizeof(*desc));
                if (uc->valid == 1) {
                    st->firmware_alive = true;
                    st->firmware_running = true;
                    log_info("iwlwifi", "microcode alive major=%u minor=%u subtype=%u errptr=%08x",
                             uc->major, uc->minor, uc->subtype, uc->errptr);
                } else {
                    log_warn("iwlwifi", "microcode alive notification was invalid (%u)",
                             uc->valid);
                }
            }
            break;
        case IWL_RX_TYPE_START_SCAN:
            st->scanning = true;
            log_info("iwlwifi", "scan started");
            break;
        case IWL_RX_TYPE_STOP_SCAN:
            st->scanning = false;
            log_info("iwlwifi", "scan stopped");
            break;
        case IWL_RX_TYPE_SCAN_RESULT:
            log_info("iwlwifi", "scan result notification");
            break;
        case IWL_RX_TYPE_STATE_CHANGED:
            log_info("iwlwifi", "card state changed");
            break;
        case IWL_RX_TYPE_MPDU_RX_DONE:
            iwl_handle_mpdu(st, buf + sizeof(*desc), len - sizeof(*desc));
            break;
        default:
            break;
        }

        memset(buf, 0, len);
        st->rx_index = (uint16_t)((st->rx_index + 1) % IWL_RX_RING_COUNT);
    }

    uint16_t wp = st->rx_index == 0 ? (uint16_t)(IWL_RX_RING_COUNT - 1)
                                    : (uint16_t)(st->rx_index - 1);
    iwl_write32(st, FH_RX_WPTR, wp & ~7u);
}

static int iwl_send_cmd(struct iwlwifi_state *st, uint8_t code,
                        const void *payload, size_t payload_len,
                        bool wait_response)
{
    if (!st->cmd_queue_ready || payload_len > IWL_CMD_DATA_SIZE) {
        return -1;
    }
    uint16_t index = st->cmd_index;
    struct iwl_tx_desc *desc =
        (struct iwl_tx_desc *)((uint8_t *)st->cmd_desc + index * IWL_TX_DESC_SIZE);
    struct iwl_tx_cmd *cmd =
        (struct iwl_tx_cmd *)((uint8_t *)st->cmd_ring + index * sizeof(struct iwl_tx_cmd));
    uintptr_t cmd_phys = st->cmd_ring_phys + index * sizeof(struct iwl_tx_cmd);
    size_t total = sizeof(*cmd) - IWL_CMD_DATA_SIZE + payload_len;

    memset(desc, 0, sizeof(*desc));
    memset(cmd, 0, sizeof(*cmd));
    cmd->code = code;
    cmd->qid = IWL_CMD_QUEUE;
    cmd->idx = (uint8_t)index;
    if (payload && payload_len) {
        memcpy(cmd->data, payload, payload_len);
    }

    desc->nsegs = 1;
    desc->segs[0].addr = (uint32_t)cmd_phys;
    desc->segs[0].len = (uint16_t)((total << 4) | ((cmd_phys >> 32) & 0xf));

    st->command_done = false;
    st->command_done_code = 0;
    st->cmd_index = (uint16_t)((index + 1) % IWL_TX_RING_COUNT);
    iwl_write32(st, CSR_HBUS_TARG_WRPTR, (IWL_CMD_QUEUE << 8) | st->cmd_index);

    if (!wait_response) {
        return 0;
    }

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 100) {
        iwl_poll_rx_notifications(st);
        if (st->command_done) {
            return st->command_done_code == code ? 0 : 1;
        }
        __asm__ volatile("pause");
    }
    log_warn("iwlwifi", "command 0x%02x timed out", code);
    return -2;
}

static int iwl_send_scan_rxon(struct iwlwifi_state *st, const struct net_iface *iface)
{
    if (!st || !iface) {
        return -1;
    }

    struct iwl_rxon rxon;
    memset(&rxon, 0, sizeof(rxon));
    memcpy(rxon.myaddr, iface->mac, IEEE80211_ADDR_LEN);
    iwl_mac_broadcast(rxon.bssid);
    iwl_mac_broadcast(rxon.wlap);
    rxon.mode = IWL_MODE_STA;
    rxon.rxchain = iwl_default_rxchain();
    rxon.ofdm_mask = 0xff;
    rxon.cck_mask = 0x0f;
    rxon.flags = IWL_RXON_24GHZ | IWL_RXON_CCK | IWL_RXON_AUTO;
    rxon.filter = IWL_FILTER_MULTICAST | IWL_FILTER_NODECRYPT | IWL_FILTER_BEACON;
    rxon.chan = 1;
    rxon.ht_single_mask = 0xff;
    rxon.ht_dual_mask = 0xff;
    rxon.ht_triple_mask = 0xff;

    int rc = iwl_send_cmd(st, IWL_CMD_RXON, &rxon, sizeof(rxon), true);
    if (rc < 0) {
        log_warn("iwlwifi", "%s RXON scan config failed (%d)", iface->name, rc);
        return rc;
    }
    return 0;
}

static size_t iwl_build_probe_request(uint8_t *out, size_t out_size,
                                      const uint8_t mac[IEEE80211_ADDR_LEN],
                                      const char *ssid)
{
    if (!out || out_size < sizeof(struct ieee80211_hdr3) + 16) {
        return 0;
    }

    struct ieee80211_hdr3 *hdr = (struct ieee80211_hdr3 *)out;
    memset(hdr, 0, sizeof(*hdr));
    hdr->fc[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_PROBE_REQ;
    iwl_mac_broadcast(hdr->addr1);
    memcpy(hdr->addr2, mac, IEEE80211_ADDR_LEN);
    iwl_mac_broadcast(hdr->addr3);

    uint8_t *p = out + sizeof(*hdr);
    uint8_t ssid_len = 0;
    if (ssid) {
        size_t raw_len = strlen(ssid);
        ssid_len = raw_len > 32 ? 32 : (uint8_t)raw_len;
    }
    *p++ = IEEE80211_ELEMID_SSID;
    *p++ = ssid_len;
    if (ssid_len) {
        memcpy(p, ssid, ssid_len);
        p += ssid_len;
    }

    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    *p++ = 1;
    *p++ = sizeof(rates);
    memcpy(p, rates, sizeof(rates));
    p += sizeof(rates);

    return (size_t)(p - out);
}

static int iwl_send_scan_command(struct iwlwifi_state *st, const struct net_iface *iface,
                                 const char *ssid)
{
    if (!st || !iface) {
        return -1;
    }

    uint8_t buf[IWL_CMD_DATA_SIZE];
    memset(buf, 0, sizeof(buf));
    struct iwl_scan_hdr *hdr = (struct iwl_scan_hdr *)buf;
    hdr->quiet_time = 10;
    hdr->quiet_threshold = 1;
    hdr->crc_threshold = ssid && ssid[0] ? 1 : IWL_SCAN_CRC_NEVER;
    hdr->rxchain = iwl_default_rxchain();
    hdr->max_svc = 250 * 1024;
    hdr->pause_svc = (4u << 22) | (100u * 1024);
    hdr->flags = IWL_RXON_24GHZ | IWL_RXON_AUTO;
    hdr->filter = IWL_FILTER_MULTICAST | IWL_FILTER_BEACON;

    struct iwl_cmd_data *tx = (struct iwl_cmd_data *)(hdr + 1);
    tx->flags = IWL_TX_AUTO_SEQ;
    tx->rate = 10 | IWL_RFLAG_CCK | IWL_RFLAG_ANT(1);
    tx->id = 15;
    tx->lifetime = IWL_LIFETIME_INFINITE;

    struct iwl_scan_essid *essid = (struct iwl_scan_essid *)(tx + 1);
    if (ssid && ssid[0]) {
        size_t raw_len = strlen(ssid);
        uint8_t ssid_len = raw_len > 32 ? 32 : (uint8_t)raw_len;
        essid[0].id = IEEE80211_ELEMID_SSID;
        essid[0].len = ssid_len;
        memcpy(essid[0].data, ssid, ssid_len);
    }

    uint8_t *probe = (uint8_t *)(essid + 20);
    size_t probe_len = iwl_build_probe_request(probe,
                                               sizeof(buf) - (size_t)(probe - buf),
                                               iface->mac, ssid);
    if (probe_len == 0) {
        return -2;
    }
    tx->len = (uint16_t)probe_len;

    static const uint8_t channels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    struct iwl_scan_chan *chan = (struct iwl_scan_chan *)(probe + probe_len);
    if ((uint8_t *)(chan + sizeof(channels) / sizeof(channels[0])) > buf + sizeof(buf)) {
        return -3;
    }
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        chan[i].flags = (ssid && ssid[0]) ? (IWL_CHAN_ACTIVE | IWL_CHAN_NPBREQS(1))
                                          : IWL_CHAN_PASSIVE;
        chan[i].chan = channels[i];
        chan[i].rf_gain = 0x28;
        chan[i].dsp_gain = 0x6e;
        chan[i].active = 30;
        chan[i].passive = 120;
    }
    hdr->nchan = (uint8_t)(sizeof(channels) / sizeof(channels[0]));
    hdr->len = (uint16_t)((uint8_t *)(chan + hdr->nchan) - buf);

    st->scanning = true;
    st->ap_count = 0;
    memset(st->aps, 0, sizeof(st->aps));
    int rc = iwl_send_cmd(st, IWL_CMD_SCAN, buf, hdr->len, true);
    if (rc < 0) {
        st->scanning = false;
        log_warn("iwlwifi", "%s scan command failed (%d)", iface->name, rc);
        return rc;
    }
    return 0;
}

static const struct iwlwifi_ap *iwl_find_ap(const struct iwlwifi_state *st,
                                            const char *ssid)
{
    if (!st || !ssid) {
        return NULL;
    }
    for (size_t i = 0; i < IWLWIFI_AP_CACHE_MAX; i++) {
        const struct iwlwifi_ap *ap = &st->aps[i];
        if (ap->valid && strcmp(ap->ssid, ssid) == 0) {
            return ap;
        }
    }
    return NULL;
}

static int iwl_scan_for_ssid(struct iwlwifi_state *st, const struct net_iface *iface,
                             const char *ssid)
{
    int rc = iwl_send_scan_rxon(st, iface);
    if (rc < 0) {
        return rc;
    }
    rc = iwl_send_scan_command(st, iface, ssid);
    if (rc < 0) {
        return rc;
    }

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 700) {
        iwl_poll_rx_notifications(st);
        if (iwl_find_ap(st, ssid)) {
            break;
        }
        if (!st->scanning && st->ap_count > 0) {
            break;
        }
        __asm__ volatile("pause");
    }
    st->scanning = false;
    return iwl_find_ap(st, ssid) ? 0 : -1;
}

static int iwl_send_bss_rxon(struct iwlwifi_state *st, const struct net_iface *iface,
                             const struct iwlwifi_ap *ap)
{
    if (!st || !iface || !ap || ap->channel == 0) {
        return -1;
    }

    struct iwl_rxon rxon;
    memset(&rxon, 0, sizeof(rxon));
    memcpy(rxon.myaddr, iface->mac, IEEE80211_ADDR_LEN);
    memcpy(rxon.bssid, ap->bssid, IEEE80211_ADDR_LEN);
    memcpy(rxon.wlap, ap->bssid, IEEE80211_ADDR_LEN);
    rxon.mode = IWL_MODE_STA;
    rxon.rxchain = iwl_default_rxchain();
    rxon.cck_mask = 0x0f;
    rxon.ofdm_mask = 0x15;
    rxon.flags = IWL_RXON_TSF | IWL_RXON_CTS_TO_SELF |
                 IWL_RXON_AUTO | IWL_RXON_24GHZ;
    rxon.filter = IWL_FILTER_MULTICAST | IWL_FILTER_NODECRYPT |
                  IWL_FILTER_BEACON | IWL_FILTER_BSS;
    rxon.chan = ap->channel;
    rxon.ht_single_mask = 0xff;
    rxon.ht_dual_mask = 0xff;
    rxon.ht_triple_mask = 0xff;

    int rc = iwl_send_cmd(st, IWL_CMD_RXON, &rxon, sizeof(rxon), true);
    if (rc < 0) {
        log_warn("iwlwifi", "%s RXON BSS config failed for %s (%d)",
                 iface->name, ap->ssid, rc);
        return rc;
    }
    memcpy(st->bssid, ap->bssid, IEEE80211_ADDR_LEN);
    st->channel = ap->channel;
    strncpy(st->associated_ssid, ap->ssid, sizeof(st->associated_ssid) - 1);
    st->associated_ssid[sizeof(st->associated_ssid) - 1] = '\0';
    return 0;
}

static int iwl_send_mgmt_frame(struct iwlwifi_state *st, const uint8_t *frame,
                               size_t frame_len, bool need_ack)
{
    if (!st || !st->tx_queue_ready || !frame || frame_len == 0 ||
        frame_len > IWL_CMD_DATA_SIZE - sizeof(struct iwl_cmd_data) - 4) {
        return -1;
    }

    uint16_t index = st->tx_index;
    struct iwl_tx_desc *desc =
        (struct iwl_tx_desc *)((uint8_t *)st->tx_desc + index * IWL_TX_DESC_SIZE);
    struct iwl_tx_cmd *cmd =
        (struct iwl_tx_cmd *)((uint8_t *)st->tx_ring + index * sizeof(struct iwl_tx_cmd));
    uintptr_t cmd_phys = st->tx_ring_phys + index * sizeof(struct iwl_tx_cmd);
    struct iwl_cmd_data *tx = (struct iwl_cmd_data *)cmd->data;
    size_t hdr_len = sizeof(struct ieee80211_hdr3);
    size_t pad = (hdr_len & 3) ? 4 - (hdr_len & 3) : 0;
    size_t inline_len = sizeof(*tx) + frame_len + pad;
    size_t total = 4 + inline_len;

    memset(desc, 0, sizeof(*desc));
    memset(cmd, 0, sizeof(*cmd));
    cmd->code = IWL_CMD_TX_DATA;
    cmd->qid = IWL_MGMT_QUEUE;
    cmd->idx = (uint8_t)index;

    tx->len = (uint16_t)frame_len;
    tx->flags = IWL_TX_AUTO_SEQ | (need_ack ? IWL_TX_NEED_ACK : 0);
    if (pad) {
        tx->flags |= IWL_TX_NEED_PADDING;
    }
    tx->id = 15;
    tx->linkq = 0;
    tx->rts_ntries = 7;
    tx->data_ntries = 7;
    tx->lifetime = IWL_LIFETIME_INFINITE;
    tx->rate = 10 | IWL_RFLAG_CCK | IWL_RFLAG_ANT(1);
    tx->security = 0;
    tx->timeout = 3;
    memcpy((uint8_t *)(tx + 1), frame, frame_len);

    desc->nsegs = 1;
    desc->segs[0].addr = (uint32_t)cmd_phys;
    desc->segs[0].len = (uint16_t)((total << 4) | ((cmd_phys >> 32) & 0xf));

    st->tx_index = (uint16_t)((index + 1) % IWL_TX_RING_COUNT);
    iwl_write32(st, CSR_HBUS_TARG_WRPTR, (IWL_MGMT_QUEUE << 8) | st->tx_index);
    return 0;
}

static size_t iwl_build_auth_frame(uint8_t *out, size_t out_size,
                                   const struct net_iface *iface,
                                   const struct iwlwifi_ap *ap,
                                   uint16_t seq)
{
    if (!out || out_size < sizeof(struct ieee80211_hdr3) + 6 || !iface || !ap) {
        return 0;
    }
    struct ieee80211_hdr3 *hdr = (struct ieee80211_hdr3 *)out;
    memset(hdr, 0, sizeof(*hdr));
    hdr->fc[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_AUTH;
    memcpy(hdr->addr1, ap->bssid, IEEE80211_ADDR_LEN);
    memcpy(hdr->addr2, iface->mac, IEEE80211_ADDR_LEN);
    memcpy(hdr->addr3, ap->bssid, IEEE80211_ADDR_LEN);
    put_le16(hdr->seq, (uint16_t)(seq << 4));

    uint8_t *body = out + sizeof(*hdr);
    put_le16(body, IEEE80211_AUTH_ALG_OPEN);
    put_le16(body + 2, IEEE80211_AUTH_TRANSACTION_REQ);
    put_le16(body + 4, 0);
    return sizeof(*hdr) + 6;
}

static uint8_t *iwl_append_rates(uint8_t *p)
{
    static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    *p++ = IEEE80211_ELEMID_RATES;
    *p++ = sizeof(rates);
    memcpy(p, rates, sizeof(rates));
    return p + sizeof(rates);
}

static uint8_t *iwl_append_wpa2_psk_ccmp_rsn(uint8_t *p)
{
    *p++ = IEEE80211_ELEMID_RSN;
    *p++ = 20;
    put_le16(p, 1);
    p += 2;
    static const uint8_t ccmp_suite[4] = { 0x00, 0x0f, 0xac, 0x04 };
    static const uint8_t psk_suite[4] = { 0x00, 0x0f, 0xac, 0x02 };
    memcpy(p, ccmp_suite, sizeof(ccmp_suite));
    p += 4;
    put_le16(p, 1);
    p += 2;
    memcpy(p, ccmp_suite, sizeof(ccmp_suite));
    p += 4;
    put_le16(p, 1);
    p += 2;
    memcpy(p, psk_suite, sizeof(psk_suite));
    p += 4;
    put_le16(p, 0);
    return p + 2;
}

static size_t iwl_build_assoc_frame(uint8_t *out, size_t out_size,
                                    const struct net_iface *iface,
                                    const struct iwlwifi_ap *ap,
                                    const char *passphrase,
                                    uint16_t seq)
{
    if (!out || out_size < sizeof(struct ieee80211_hdr3) + 64 || !iface || !ap) {
        return 0;
    }
    struct ieee80211_hdr3 *hdr = (struct ieee80211_hdr3 *)out;
    memset(hdr, 0, sizeof(*hdr));
    hdr->fc[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_ASSOC_REQ;
    memcpy(hdr->addr1, ap->bssid, IEEE80211_ADDR_LEN);
    memcpy(hdr->addr2, iface->mac, IEEE80211_ADDR_LEN);
    memcpy(hdr->addr3, ap->bssid, IEEE80211_ADDR_LEN);
    put_le16(hdr->seq, (uint16_t)(seq << 4));

    uint8_t *p = out + sizeof(*hdr);
    uint16_t cap = IEEE80211_CAPINFO_ESS;
    if (ap->privacy || (passphrase && passphrase[0])) {
        cap |= IEEE80211_CAPINFO_PRIVACY;
    }
    put_le16(p, cap);
    p += 2;
    put_le16(p, 100);
    p += 2;
    *p++ = IEEE80211_ELEMID_SSID;
    *p++ = ap->ssid_len;
    memcpy(p, ap->ssid, ap->ssid_len);
    p += ap->ssid_len;
    p = iwl_append_rates(p);
    if (ap->privacy || (passphrase && passphrase[0])) {
        p = iwl_append_wpa2_psk_ccmp_rsn(p);
    }
    return (size_t)(p - out);
}

static int iwl_wait_for_auth(struct iwlwifi_state *st)
{
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 300) {
        iwl_poll_rx_notifications(st);
        if (st->auth_done) {
            return st->auth_status == IEEE80211_STATUS_SUCCESS ? 0 : -2;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int iwl_wait_for_assoc(struct iwlwifi_state *st)
{
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 300) {
        iwl_poll_rx_notifications(st);
        if (st->assoc_done) {
            return st->assoc_status == IEEE80211_STATUS_SUCCESS ? 0 : -2;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int iwl_install_pairwise_ccmp_key(struct iwlwifi_state *st,
                                         const struct iwlwifi_ap *ap)
{
    if (!st || !ap || !st->wpa_ptk_ready) {
        return -1;
    }
    struct iwl_node_info node;
    memset(&node, 0, sizeof(node));
    node.control = IWL_NODE_UPDATE;
    memcpy(node.macaddr, ap->bssid, IEEE80211_ADDR_LEN);
    node.id = IWL_ID_BSS;
    node.flags = IWL_FLAG_SET_KEY;
    node.kflags = IWL_KFLAG_CCMP | IWL_KFLAG_MAP;
    memcpy(node.key, st->wpa_ptk + 32, 16);
    int rc = iwl_send_cmd(st, IWL_CMD_ADD_NODE, &node, sizeof(node), true);
    if (rc < 0) {
        log_warn("iwlwifi", "pairwise CCMP key install failed (%d)", rc);
        return rc;
    }
    log_info("iwlwifi", "installed pairwise CCMP key for %02x:%02x:%02x:%02x:%02x:%02x",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    return 0;
}

static int iwl_dma_load_section(struct iwlwifi_state *st, uint32_t dst,
                                const uint8_t *data, uint32_t size)
{
    if (!data || size == 0 || size > st->firmware_dma_size) {
        return -1;
    }
    memcpy(st->firmware_dma, data, size);
    if (iwl_nic_lock(st) < 0) {
        return -2;
    }

    iwl_write32(st, FH_TX_CONFIG(IWL_SRVC_DMACHNL), IWL_FH_TX_CONFIG_DMA_PAUSE);
    iwl_write32(st, FH_SRAM_ADDR(IWL_SRVC_DMACHNL), dst);
    iwl_write32(st, FH_TFBD_CTRL0(IWL_SRVC_DMACHNL),
                (uint32_t)st->firmware_dma_phys);
    iwl_write32(st, FH_TFBD_CTRL1(IWL_SRVC_DMACHNL),
                (uint32_t)(((st->firmware_dma_phys >> 32) & 0xf) << 28) | size);
    iwl_write32(st, FH_TXBUF_STATUS(IWL_SRVC_DMACHNL),
                IWL_FH_TXBUF_TBNUM(1) | IWL_FH_TXBUF_TBIDX(1) | IWL_FH_TXBUF_VALID);
    iwl_write32(st, CSR_INT, 0xffffffffu);
    iwl_write32(st, CSR_FH_INT_STATUS, 0xffffffffu);
    iwl_write32(st, FH_TX_CONFIG(IWL_SRVC_DMACHNL),
                IWL_FH_TX_CONFIG_DMA_ENA | IWL_FH_TX_CONFIG_HOST_END);
    iwl_nic_unlock(st);

    uint32_t seen = 0;
    if (!iwl_wait_int(st, IWL_INT_FH_TX | IWL_FH_INT_TX_CHNL(0) |
                          IWL_FH_INT_TX_CHNL(1), &seen, 500)) {
        log_warn("iwlwifi", "firmware DMA section dst=%08x size=%u timed out/int=%08x",
                 dst, size, seen);
        return -3;
    }
    return 0;
}

static bool iwl_uses_legacy_4965_path(const struct iwlwifi_state *st)
{
    return strcmp(st->family, "4965") == 0;
}

static int iwl_execute_runtime_firmware(struct iwlwifi_state *st)
{
    if (!st->firmware_parsed || !st->firmware_staged) {
        return -1;
    }
    if (st->modern_transport) {
        log_warn("iwlwifi", "%s uses the newer iwm/iwlwifi transport; firmware is present but command/RX/TX rings are not complete yet",
                 st->family);
        return -6;
    }
    if (iwl_uses_legacy_4965_path(st)) {
        log_warn("iwlwifi", "4965 bootcode path is not enabled yet");
        return -2;
    }
    int apm_rc = iwl_apm_init(st);
    if (apm_rc < 0) {
        return -7;
    }
    int rx_rc = iwl_init_rx_ring(st);
    if (rx_rc < 0) {
        log_warn("iwlwifi", "could not initialize RX ring (%d)", rx_rc);
        return -5;
    }
    int cmd_rc = iwl_init_cmd_queue(st);
    if (cmd_rc < 0) {
        log_warn("iwlwifi", "could not initialize command queue (%d)", cmd_rc);
        return -8;
    }
    int tx_rc = iwl_init_tx_queue(st);
    if (tx_rc < 0) {
        log_warn("iwlwifi", "could not initialize management TX queue (%d)", tx_rc);
        return -9;
    }

    iwl_write32(st, CSR_INT_MASK, IWL_INT_MASK_MIN);
    iwl_write32(st, CSR_INT, 0xffffffffu);
    iwl_write32(st, CSR_FH_INT_STATUS, 0xffffffffu);

    int rc = iwl_dma_load_section(st, IWL_FW_TEXT_BASE,
                                  st->runtime_ucode.text,
                                  st->runtime_ucode.text_size);
    if (rc < 0) {
        return rc;
    }
    rc = iwl_dma_load_section(st, IWL_FW_DATA_BASE,
                              st->runtime_ucode.data,
                              st->runtime_ucode.data_size);
    if (rc < 0) {
        return rc;
    }

    iwl_write32(st, CSR_INT, 0xffffffffu);
    iwl_write32(st, CSR_RESET, 0);

    uint32_t seen = 0;
    if (!iwl_wait_int(st, IWL_INT_ALIVE, &seen, 100)) {
        log_warn("iwlwifi", "runtime firmware did not signal alive (int=%08x)", seen);
        return -4;
    }
    uint64_t start = pit_ticks();
    while (!st->firmware_alive && pit_ticks() - start < 100) {
        iwl_poll_rx_notifications(st);
        __asm__ volatile("pause");
    }
    if (!st->firmware_alive) {
        st->firmware_running = true;
        st->firmware_alive = true;
        log_warn("iwlwifi", "runtime firmware raised ALIVE interrupt but no UC_READY notification was parsed");
    } else {
        log_info("iwlwifi", "runtime firmware signaled alive");
    }
    return 0;
}

static void iwl_probe_internal_bus(const struct iwlwifi_state *st, const char *ifname)
{
    if (!st->firmware_staged) {
        return;
    }
    if (iwl_nic_lock(st) < 0) {
        log_warn("iwlwifi", "%s could not obtain MAC access lock for firmware bring-up",
                 ifname);
        return;
    }
    uint32_t bsm = iwl_prph_read(st, IWL_BSM_WR_CTRL);
    iwl_nic_unlock(st);
    log_info("iwlwifi", "%s internal PRPH bus reachable, BSM_WR_CTRL=%08x",
             ifname, bsm);
}

int iwlwifi_start(struct net_iface *iface)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0) {
        return -1;
    }
    struct iwlwifi_state *st = iface->driver_data;
    if (!st || !st->attached) {
        return -1;
    }
    if (st->firmware_alive) {
        return 0;
    }
    int rc = iwl_execute_runtime_firmware(st);
    if (rc == 0) {
        iface->up = true;
    }
    return rc;
}

static int iwlwifi_transmit(struct net_iface *iface, const void *frame, size_t len)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0) {
        return -1;
    }
    struct iwlwifi_state *st = iface->driver_data;
    if (!st || !st->associated || !st->link_ready) {
        return -2;
    }
    if (!frame || len < 14 || len > 1500) {
        return -1;
    }
    const uint8_t *eth = frame;
    uint16_t ethertype = ((uint16_t)eth[12] << 8) | eth[13];
    uint8_t wifi[1700];
    struct ieee80211_hdr3 *hdr = (struct ieee80211_hdr3 *)wifi;
    memset(hdr, 0, sizeof(*hdr));
    hdr->fc[0] = IEEE80211_FC0_TYPE_DATA;
    hdr->fc[1] = IEEE80211_FC1_DIR_TODS;
    if (st->wpa_ptk_ready) {
        hdr->fc[1] |= IEEE80211_FC1_PROTECTED;
    }
    memcpy(hdr->addr1, st->bssid, 6);
    memcpy(hdr->addr2, iface->mac, 6);
    memcpy(hdr->addr3, eth, 6);
    put_le16(hdr->seq, (uint16_t)(st->tx_sequence++ << 4));
    uint8_t *p = wifi + sizeof(*hdr);
    static const uint8_t llc_snap[] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0, 0 };
    memcpy(p, llc_snap, sizeof(llc_snap));
    p[6] = (uint8_t)(ethertype >> 8);
    p[7] = (uint8_t)ethertype;
    p += sizeof(llc_snap);
    memcpy(p, eth + 14, len - 14);
    p += len - 14;
    int rc = iwl_send_mgmt_frame(st, wifi, (size_t)(p - wifi), true);
    if (rc == 0) {
        iface->tx_packets++;
        iface->tx_bytes += len;
    }
    return rc;
}

static void iwlwifi_poll(struct net_iface *iface, net_rx_callback_t callback, void *ctx)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0) {
        return;
    }
    struct iwlwifi_state *st = iface->driver_data;
    if (!st) {
        return;
    }
    st->rx_callback = callback;
    st->rx_callback_ctx = ctx;
    iwl_poll_rx_notifications(st);
    st->rx_callback = NULL;
    st->rx_callback_ctx = NULL;
}

int iwlwifi_scan(struct net_iface *iface)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0) {
        return -1;
    }
    struct iwlwifi_state *st = iface->driver_data;
    if (!st || !st->attached) {
        return -1;
    }
    int rc = iwlwifi_start(iface);
    if (rc < 0) {
        return rc;
    }
    if (st->modern_transport) {
        log_warn("iwlwifi", "%s scan blocked: modern firmware command queue is not implemented",
                 iface->name);
        return -6;
    }

    rc = iwl_send_scan_rxon(st, iface);
    if (rc < 0) {
        return -9;
    }
    rc = iwl_send_scan_command(st, iface, NULL);
    if (rc < 0) {
        return -9;
    }

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 700) {
        iwl_poll_rx_notifications(st);
        if (!st->scanning && st->ap_count > 0) {
            break;
        }
        __asm__ volatile("pause");
    }
    st->scanning = false;

    if (st->ap_count == 0) {
        log_warn("iwlwifi", "%s scan completed but no AP beacons/probe responses were parsed",
                 iface->name);
        return -10;
    }

    log_info("iwlwifi", "%s scan found %u AP(s)", iface->name, (uint32_t)st->ap_count);
    return 0;
}

int iwlwifi_associate(struct net_iface *iface, const char *ssid, const char *passphrase)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0 || !ssid || !ssid[0]) {
        return -1;
    }
    struct iwlwifi_state *st = iface->driver_data;
    if (!st || !st->attached) {
        return -1;
    }
    int rc = iwlwifi_start(iface);
    if (rc < 0) {
        return rc;
    }
    if (passphrase && passphrase[0]) {
        log_warn("iwlwifi", "%s cannot associate with '%s': WPA supplicant/4-way handshake is pending",
                 iface->name, ssid);
        return -8;
    }
    if (st->modern_transport) {
        log_warn("iwlwifi", "%s cannot associate with '%s': modern MAC context/station commands are pending",
                 iface->name, ssid);
        return -6;
    }

    const struct iwlwifi_ap *ap = iwl_find_ap(st, ssid);
    if (!ap) {
        int scan = iwl_scan_for_ssid(st, iface, ssid);
        if (scan < 0) {
            log_warn("iwlwifi", "%s could not find SSID '%s' during directed scan",
                     iface->name, ssid);
            return -10;
        }
        ap = iwl_find_ap(st, ssid);
    }
    if (!ap) {
        return -10;
    }

    int rxon = iwl_send_bss_rxon(st, iface, ap);
    if (rxon < 0) {
        return -7;
    }

    uint8_t frame[512];
    st->auth_done = false;
    st->auth_status = 0xffff;
    size_t frame_len = iwl_build_auth_frame(frame, sizeof(frame), iface, ap,
                                            st->tx_sequence++);
    if (frame_len == 0 || iwl_send_mgmt_frame(st, frame, frame_len, true) < 0) {
        log_warn("iwlwifi", "%s could not transmit auth frame for '%s'",
                 iface->name, ssid);
        return -11;
    }
    int auth = iwl_wait_for_auth(st);
    if (auth < 0) {
        log_warn("iwlwifi", "%s auth failed for '%s' (%d status=%u)",
                 iface->name, ssid, auth, st->auth_status);
        return -11;
    }

    st->assoc_done = false;
    st->assoc_status = 0xffff;
    st->assoc_id = 0;
    frame_len = iwl_build_assoc_frame(frame, sizeof(frame), iface, ap, passphrase,
                                      st->tx_sequence++);
    if (frame_len == 0 || iwl_send_mgmt_frame(st, frame, frame_len, true) < 0) {
        log_warn("iwlwifi", "%s could not transmit assoc frame for '%s'",
                 iface->name, ssid);
        return -12;
    }
    int assoc = iwl_wait_for_assoc(st);
    if (assoc < 0) {
        log_warn("iwlwifi", "%s association failed for '%s' (%d status=%u)",
                 iface->name, ssid, assoc, st->assoc_status);
        return -12;
    }

    if (ap->privacy || (passphrase && passphrase[0])) {
        if (!passphrase || !passphrase[0]) {
            log_warn("iwlwifi", "%s needs a WPA passphrase for '%s'", iface->name, ssid);
            return -8;
        }
        tnu_wpa_pmk_from_passphrase(passphrase, ssid, st->wpa_pmk);
        iwl_make_snonce(st, iface);
        st->wpa_key_msg1 = false;
        st->wpa_key_msg3 = false;
        uint64_t wpa_start = pit_ticks();
        while (!st->wpa_key_msg1 && pit_ticks() - wpa_start < 1000) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->wpa_key_msg1) {
            log_warn("iwlwifi", "%s timed out waiting for WPA msg1 from '%s'",
                     iface->name, ssid);
            return -14;
        }
        wpa_start = pit_ticks();
        while (!st->wpa_key_msg3 && pit_ticks() - wpa_start < 1000) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->wpa_key_msg3) {
            log_warn("iwlwifi", "%s timed out waiting for WPA msg3 from '%s'",
                     iface->name, ssid);
            return -15;
        }
        if (iwl_install_pairwise_ccmp_key(st, ap) < 0) {
            return -16;
        }
        log_warn("iwlwifi", "%s WPA PTK installed for '%s'; GTK unwrap/msg4 and data path are next",
                 iface->name, ssid);
        return -17;
    }

    st->associated = true;
    st->link_ready = true;
    iface->link = true;
    iface->up = true;
    log_info("iwlwifi", "%s associated with open SSID '%s' aid=%u; data path enabled",
             iface->name, ssid, st->assoc_id);
    return 0;
}

bool iwlwifi_is_supported(const struct pci_device *dev)
{
    return dev && dev->vendor_id == 0x8086 && dev->class_code == 0x02 &&
           dev->subclass == 0x80 && find_device(dev->device_id) != NULL;
}

int iwlwifi_attach(struct net_iface *iface, const struct pci_device *dev)
{
    if (!iface || !dev || !iwlwifi_is_supported(dev) || state_count >= IWL_MAX_DEVICES) {
        return -1;
    }

    uint32_t bar0 = dev->bars[0];
    if ((bar0 & 1u) || bar0 == 0 || bar0 == 0xffffffffu) {
        log_warn("iwlwifi", "%s has no usable MMIO BAR0", iface->name);
        return -1;
    }

    uintptr_t mmio = (uintptr_t)(bar0 & ~0x0fu);
    if (vmm_map_range_identity(mmio, IWL_MMIO_SIZE, 0) < 0) {
        log_warn("iwlwifi", "%s BAR0 %p could not be mapped", iface->name, (void *)mmio);
        return -1;
    }

    const struct iwl_device_info *info = find_device(dev->device_id);
    struct iwlwifi_state *st = &states[state_count++];
    memset(st, 0, sizeof(*st));
    st->mmio = mmio;
    st->iface = iface;
    st->device_id = dev->device_id;
    st->family = info->family;
    st->firmware_name = info->firmware;
    st->modern_transport = iwl_device_uses_modern_transport(info);

    pci_enable_bus_mastering(dev);

    iwl_write32(st, CSR_INT_MASK, 0);
    iwl_write32(st, CSR_INT, 0xffffffffu);
    iwl_write32(st, CSR_FH_INT_STATUS, 0xffffffffu);

    st->hw_rev = iwl_read32(st, CSR_HW_REV);
    st->rf_id = iwl_read32(st, CSR_HW_RF_ID);
    st->gp_cntrl = iwl_read32(st, CSR_GP_CNTRL);
    st->attached = true;
    iwl_load_firmware(st);
    iwl_probe_internal_bus(st, iface->name);

    iface->driver = "iwlwifi";
    iface->driver_data = st;
    iface->ops = &iwlwifi_net_ops;
    iface->configurable = false;
    iface->up = false;
    iface->link = false;

    log_info("iwlwifi", "%s Intel %s device=%04x mmio=%p hw_rev=%08x rf_id=%08x",
             iface->name, st->family, dev->device_id, (void *)mmio, st->hw_rev, st->rf_id);
    if (st->firmware_loaded && st->modern_transport) {
        log_warn("iwlwifi", "%s firmware is available; this device needs the newer command/RX/TX transport before association",
                 iface->name);
    } else if (st->firmware_loaded) {
        log_warn("iwlwifi", "%s firmware is staged; DMA upload, queues, 802.11 MAC, and WPA are next",
                 iface->name);
    } else {
        log_warn("iwlwifi", "%s needs firmware %s plus 802.11/WPA layers before association",
                 iface->name, st->firmware_name);
    }
    return 0;
}

const struct iwlwifi_state *iwlwifi_state_for(const struct net_iface *iface)
{
    if (!iface || strcmp(iface->driver, "iwlwifi") != 0) {
        return NULL;
    }
    return iface->driver_data;
}
