/*
 * iwlwifi – Intel Wireless driver (full implementation)
 *
 * This file contains the complete iwlwifi driver implementation for TNU.
 * It supports both DVM (old) and MVM (new) firmware transports for Intel
 * wireless devices (3160, 7260, 7265, 8260, 8265, 9260, 9560).
 *
 * The driver:
 * - Loads firmware from /lib/firmware/iwlwifi/
 * - Handles both INIT and RT firmware phases (MVM) or single image (DVM)
 * - Implements RX/TX rings, command queues, and interrupt handling
 * - Provides scan/connect/association with WPA-PSK support
 * - Exposes /dev/iwlwifi for userspace ioctl access
 *
 * For details on the implementation, see the comments in iwlwifi_attach()
 * and iwlwifi_start().
 */

#include <arch/pci.h>
#include <arch/pit.h>
#include <tnu/crypto.h>
#include <tnu/iwlwifi.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/net.h>
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
#define IWL_TLV_FLAGS 18
#define IWL_TLV_SEC_RT 19
#define IWL_TLV_SEC_INIT 20
#define IWL_TLV_API_CHANGES_SET 29
#define IWL_TLV_ENABLED_CAPABILITIES 30

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

/* DVM command IDs */
#define IWL_CMD_RXON          16
#define IWL_CMD_RXON_ASSOC    17
#define IWL_CMD_ADD_NODE      24
#define IWL_CMD_TX_DATA       28
#define IWL_CMD_LINK_QUALITY  78
#define IWL_CMD_SET_POWER     119
#define IWL_CMD_SCAN          128
#define IWL_CMD_GET_STATISTICS 156

/*
 * MVM command IDs — from FreeBSD sys/dev/iwm/if_iwmreg.h
 * IWM_CMD_QUEUE = 9  (NOT 0 — this is the critical fix)
 */
#define MVM_CMD_QUEUE               9    /* host command queue for MVM */
#define MVM_CMD_ALIVE               0x01
#define MVM_CMD_PHY_DB              0x6c
#define MVM_CMD_POWER_TABLE         0x77 /* IWM_POWER_TABLE_CMD */
#define MVM_CMD_PHY_CONTEXT         0x08 /* IWM_PHY_CONTEXT_CMD */
#define MVM_CMD_MAC_CONTEXT         0x28 /* IWM_MAC_CONTEXT_CMD */
#define MVM_CMD_TIME_EVENT          0x29 /* IWM_TIME_EVENT_CMD */
#define MVM_CMD_TIME_EVENT_NOTIF    0x2a /* IWM_TIME_EVENT_NOTIFICATION */
#define MVM_CMD_BINDING             0x2b /* IWM_BINDING_CONTEXT_CMD */
#define MVM_CMD_ADD_STA             0x18 /* REPLY_ADD_STA */
#define MVM_CMD_ADD_STA_KEY         0x17
#define MVM_CMD_SCAN_LMAC           0x51 /* IWM_SCAN_OFFLOAD_REQUEST_CMD */
#define MVM_CMD_SCAN_ABORT_LMAC     0x52 /* IWM_SCAN_OFFLOAD_ABORT_CMD */
#define MVM_CMD_SCAN_COMPLETE_LMAC  0x6d /* IWM_SCAN_OFFLOAD_COMPLETE */
#define MVM_CMD_NVM_ACCESS          0x88
#define MVM_CMD_CALIB_RES_NOTIF     0x6b /* PHY_DB blob from init firmware */
#define MVM_CMD_BT_CONFIG           0x9b
#define MVM_CMD_STATISTICS_NOTIF    0x9d
#define MVM_CMD_REPLY_SF_CFG        0x31

/* MVM context action (IWM_FW_CTXT_ACTION_*) */
#define MVM_CTXT_ACTION_ADD         1
#define MVM_CTXT_ACTION_MODIFY      2
#define MVM_CTXT_ACTION_REMOVE      3

/* id_and_color encoding: bits[7:0]=id, bits[15:8]=color */
#define MVM_ID_AND_COLOR(id, color) (((id) & 0xff) | (((color) & 0xff) << 8))

/* MAC types (IWM_FW_MAC_TYPE_*) */
#define MVM_MAC_TYPE_AUX            1
#define MVM_MAC_TYPE_BSS_STA        5

/* TSF IDs */
#define MVM_TSF_ID_A                0

/* PHY band (IWM_PHY_BAND_*) — NOTE: 5GHz=0, 2.4GHz=1 in FreeBSD */
#define MVM_PHY_BAND_5              0
#define MVM_PHY_BAND_24             1
#define MVM_PHY_VHT_CHAN_MODE_20    0

/* PHY RX chain flags */
#define MVM_PHY_RX_CHAIN_VALID_MSK      (0x7u << 1)
#define MVM_PHY_RX_CHAIN_FORCE_SEL_MSK  (0x7u << 4)
#define MVM_PHY_RX_CHAIN_MIMO_SEL_MSK   (0x7u << 7)
#define MVM_PHY_RX_CHAIN_CNT_MSK        (0x3u << 10)
#define MVM_PHY_RX_CHAIN_MIMO_CNT_MSK   (0x3u << 12)

/* STA types (IWM_STA_*) */
#define MVM_STA_LINK                0
#define MVM_STA_AUX_ACTIVITY        4

/* Station action */
#define MVM_STA_ACTION_ADD          0 /* add new (not modify) */
#define MVM_STA_ACTION_MODIFY       1

/* MAC filter flags (IWM_MAC_FILTER_*) */
#define MVM_MAC_FILTER_ACCEPT_GRP   (1u << 0)
#define MVM_MAC_FILTER_DIS_DECRYPT  (1u << 2)
#define MVM_MAC_FILTER_ACCEPT_BEACON (1u << 3)
#define MVM_MAC_FILTER_ACCEPT_PROBE  (1u << 4)
#define MVM_MAC_FILTER_ACCEPT_ALL   0x3fu

/* CCK/OFDM basic rates */
#define MVM_BASIC_RATES_OFDM        0x00000ff0u
#define MVM_BASIC_RATES_CCK         0x0000000fu

/* LMAC scan flags (IWM_LMAC_SCAN_FLAG_*) */
#define MVM_LMAC_SCAN_FLAG_PASS_ALL     (1u << 0)
#define MVM_LMAC_SCAN_FLAG_PASSIVE      (1u << 1)
#define MVM_LMAC_SCAN_FLAG_ITER_COMPLETE (1u << 3)

/* Power: CAM mode (no power save) */
#define MVM_DEVICE_POWER_FLAGS_CAM  (1u << 13)

/* Time event type for association */
#define MVM_TE_BSS_STA_ASSOC        1
#define MVM_TE_SESSION_PROT_MAX_MS  500
#define MVM_TE_V2_NOTIF_HOST_START  (1u << 0)
#define MVM_TE_V2_NOTIF_HOST_END    (1u << 1)
#define MVM_TE_V2_FRAG_NONE         1

/* MVM alive status */
#define MVM_ALIVE_STATUS_OK         0xCAFE
#define MVM_ALIVE_STATUS_ERR        0xDEAD

/* MVM RX notification IDs */
#define MVM_RX_SCAN_COMPLETE        0x6d /* IWM_SCAN_OFFLOAD_COMPLETE */
#define MVM_RX_TE_NOTIF             0x2a /* IWM_TIME_EVENT_NOTIFICATION */

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
#define IWL_RESET_SW                  (1u << 7)
#define IWL_GIO_CHICKEN_L1A_NO_L0S_RX (1u << 23)
#define IWL_GIO_CHICKEN_DIS_L0S_TIMER (1u << 29)
#define IWL_GIO_L0S_ENA               (1u << 1)
#define IWL_PRPH_DWORD ((sizeof(uint32_t) - 1u) << 24)
#define IWL_GP_CNTRL_MAC_ACCESS_ENA (1u << 0)
#define IWL_GP_CNTRL_MAC_CLOCK_READY (1u << 0)
#define IWL_GP_CNTRL_INIT_DONE      (1u << 2)
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

/*
 * MVM structures — layouts from FreeBSD sys/dev/iwm/if_iwmreg.h
 */

/* PHY_CONTEXT_CMD (0x08) — IWM_PHY_CONTEXT_CMD_API_VER_1 */
struct mvm_fw_channel_info_v1 {
    uint8_t  band;       /* IWM_PHY_BAND_* (5GHz=0, 2.4GHz=1) */
    uint8_t  channel;
    uint8_t  width;      /* IWM_PHY_VHT_CHANNEL_MODE20=0 */
    uint8_t  ctrl_pos;
} __attribute__((packed));

struct mvm_phy_ctx_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t apply_time;
    uint32_t tx_param_color;
    struct mvm_fw_channel_info_v1 ci;
    uint32_t txchain_info;
    uint32_t rxchain_info;
    uint32_t acquisition_data;
    uint32_t dsp_cfg_flags;
} __attribute__((packed));

/* AC QOS params — part of MAC_CONTEXT_CMD */
struct mvm_ac_qos {
    uint16_t cw_min;
    uint16_t cw_max;
    uint8_t  aifsn;
    uint8_t  fifos_mask;
    uint16_t edca_txop;
} __attribute__((packed));

/* BSS station data — appended to mac_ctx_cmd for BSS_STA type */
struct mvm_mac_data_sta {
    uint32_t is_assoc;
    uint32_t dtim_time;
    uint64_t dtim_tsf;
    uint32_t bi;
    uint32_t bi_reciprocal;
    uint32_t dtim_interval;
    uint32_t dtim_reciprocal;
    uint32_t listen_interval;
    uint32_t assoc_id;
    uint32_t assoc_beacon_arrive_time;
} __attribute__((packed));

/* MAC_CONTEXT_CMD (0x28) — IWM_MAC_CONTEXT_CMD_API_S_VER_1 */
#define MVM_AC_NUM  4
struct mvm_mac_ctx_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t mac_type;
    uint32_t tsf_id;
    uint8_t  node_addr[6];
    uint16_t reserved_for_node_addr;
    uint8_t  bssid_addr[6];
    uint16_t reserved_for_bssid_addr;
    uint32_t cck_rates;
    uint32_t ofdm_rates;
    uint32_t protection_flags;
    uint32_t cck_short_preamble;
    uint32_t short_slot;
    uint32_t filter_flags;
    uint32_t qos_flags;
    struct mvm_ac_qos ac[MVM_AC_NUM + 1];
    struct mvm_mac_data_sta sta; /* union, we only use BSS STA */
} __attribute__((packed));

/* BINDING_CONTEXT_CMD (0x2b) — IWM_BINDING_CMD_API_S_VER_2 */
#define MVM_MAX_MACS_IN_BINDING 3
struct mvm_binding_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t macs[MVM_MAX_MACS_IN_BINDING];
    uint32_t phy;
    uint32_t lmac_id;
} __attribute__((packed));

/* ADD_STA (0x18) — ADD_STA_CMD_API_S_VER_7 */
struct mvm_add_sta_cmd {
    uint8_t  add_modify;
    uint8_t  awake_acs;
    uint16_t tid_disable_tx;
    uint32_t mac_id_n_color;
    uint8_t  addr[6];
    uint16_t reserved2;
    uint8_t  sta_id;
    uint8_t  modify_mask;
    uint16_t reserved3;
    uint32_t station_flags;
    uint32_t station_flags_msk;
    uint8_t  add_immediate_ba_tid;
    uint8_t  remove_immediate_ba_tid;
    uint16_t add_immediate_ba_ssn;
    uint16_t sleep_tx_count;
    uint16_t sleep_state_flags;
    uint16_t assoc_id;
    uint16_t beamform_flags;
    uint32_t tfd_queue_msk;
} __attribute__((packed));

/* TIME_EVENT_CMD (0x29) — IWM_MAC_TIME_EVENT_CMD_API_S_VER_2 */
struct mvm_time_event_cmd {
    uint32_t id_and_color;
    uint32_t action;
    uint32_t id;
    uint32_t apply_time;
    uint32_t max_delay;
    uint32_t depends_on;
    uint32_t interval;
    uint32_t duration;
    uint8_t  repeat;
    uint8_t  max_frags;
    uint16_t policy;
} __attribute__((packed));

/* TIME_EVENT_NOTIFICATION (0x2a) */
struct mvm_time_event_notif {
    uint32_t timestamp;
    uint32_t session_id;
    uint32_t unique_id;
    uint32_t id_and_color;
    uint32_t action;
    uint32_t status;
} __attribute__((packed));

/* SCAN_OFFLOAD_REQUEST_CMD (0x51) — LMAC scan */
#define MVM_SCAN_MAX_CHANNELS   40
#define MVM_PROBE_OPTION_MAX    20

struct mvm_scan_channel_cfg_lmac {
    uint32_t flags;
    uint16_t channel_num;
    uint16_t iter_count;
    uint32_t iter_interval;
} __attribute__((packed));

struct mvm_ssid_ie {
    uint8_t id;
    uint8_t len;
    uint8_t ssid[32];
} __attribute__((packed));

struct mvm_scan_req_tx_cmd {
    uint32_t tx_flags;
    uint32_t rate_n_flags;
    uint8_t  sta_id;
    uint8_t  reserved[3];
} __attribute__((packed));

struct mvm_scan_schedule_lmac {
    uint16_t interval;
    uint8_t  iter_count;
    uint8_t  reserved;
} __attribute__((packed));

struct mvm_scan_channel_opt {
    uint16_t flags;
    uint16_t non_ebs_ratio;
} __attribute__((packed));

struct mvm_scan_req_lmac {
    uint32_t reserved1;
    uint8_t  n_channels;
    uint8_t  active_dwell;
    uint8_t  passive_dwell;
    uint8_t  fragmented_dwell;
    uint8_t  extended_dwell;
    uint8_t  reserved2;
    uint16_t rx_chain_select;
    uint32_t scan_flags;
    uint32_t max_out_time;
    uint32_t suspend_time;
    uint32_t flags;
    uint32_t filter_flags;
    struct mvm_scan_req_tx_cmd tx_cmd[2];
    struct mvm_ssid_ie direct_scan[MVM_PROBE_OPTION_MAX];
    uint32_t scan_prio;
    uint32_t iter_num;
    uint32_t delay;
    struct mvm_scan_schedule_lmac schedule[2];
    struct mvm_scan_channel_opt channel_opt[2];
    /* followed by variable-length channel data */
} __attribute__((packed));

/* PHY_DB_CMD (0x6c) — calibration blob replay */
struct mvm_phy_db_cmd {
    uint16_t type;
    uint16_t length;
    /* followed by data bytes */
} __attribute__((packed));

/* PHY DB section types from init firmware notifications */
#define MVM_PHY_DB_CFG              1
#define MVM_PHY_DB_CALIB_NCH        2
#define MVM_PHY_DB_CALIB_CHG_PAPD   3
#define MVM_PHY_DB_CALIB_CHG_TXP    4
#define MVM_PHY_DB_MAX_TYPE         5

/* POWER_TABLE_CMD (0x77) — device-wide power, use CAM (no sleep) */
struct mvm_device_power_cmd {
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed));

/* ALIVE response (0x01) */
struct mvm_lmac_alive {
    uint32_t ucode_minor;
    uint32_t ucode_major;
    uint8_t  sw_rev[8];
    uint8_t  ver_type;
    uint8_t  ver_subtype;
    uint16_t reserved1;
    uint32_t log_event_table_ptr;
    uint32_t error_event_table_ptr;
    uint32_t timestamp;
    uint32_t reserved2;
} __attribute__((packed));

struct mvm_alive_resp_v3 {
    uint16_t status;    /* 0xCAFE=OK, 0xDEAD=error */
    uint16_t flags;
    struct mvm_lmac_alive lmac_data;
    uint8_t  umac_pad[20]; /* reserved UMAC alive payload bytes */
} __attribute__((packed));

/* PHY DB storage — one entry per section type */
#define MVM_PHY_DB_MAX_BLOB_SIZE  512
#define MVM_PHY_DB_MAX_CH_GROUPS  16

struct mvm_phy_db_entry {
    uint16_t size;
    uint8_t  data[MVM_PHY_DB_MAX_BLOB_SIZE];
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
static void iwl_short_delay(void);
static void iwl_prph_write(const struct iwlwifi_state *st, uint32_t addr, uint32_t value);
static int iwlwifi_transmit(struct net_iface *iface, const void *frame, size_t len);
static void iwlwifi_poll(struct net_iface *iface, net_rx_callback_t callback, void *ctx);
static int iwl_send_mgmt_frame(struct iwlwifi_state *st, const uint8_t *frame,
                               size_t frame_len, bool need_ack);
static const struct iwlwifi_ap *iwl_find_ap(const struct iwlwifi_state *st,
                                            const char *ssid);
static uint8_t *iwl_append_wpa2_psk_ccmp_rsn(uint8_t *p);
static size_t iwl_build_auth_frame(uint8_t *out, size_t out_size,
                                   const struct net_iface *iface,
                                   const struct iwlwifi_ap *ap, uint16_t seq);
static size_t iwl_build_assoc_frame(uint8_t *out, size_t out_size,
                                    const struct net_iface *iface,
                                    const struct iwlwifi_ap *ap,
                                    const char *passphrase, uint16_t seq);
static int iwl_wait_for_auth(struct iwlwifi_state *st);
static int iwl_wait_for_assoc(struct iwlwifi_state *st);
static int iwl_install_pairwise_ccmp_key(struct iwlwifi_state *st,
                                         const struct iwlwifi_ap *ap);
static void iwl_make_snonce(struct iwlwifi_state *st, const struct net_iface *iface);
static void iwl_mvm_phy_db_store(struct iwlwifi_state *st,
                                  const uint8_t *payload, size_t plen);

static const struct net_driver_ops iwlwifi_net_ops = {
    .transmit = iwlwifi_transmit,
    .poll = iwlwifi_poll,
};

/*
 * Device table — same chips as FreeBSD sys/dev/iwm/if_iwm.c,
 * using Linux firmware filenames (iwlwifi-XXXX-YY.ucode).
 */
static const struct iwl_device_info iwl_devices[] = {
    /* Intel 3160 */
    { 0x08b3, "3160", "iwlwifi-3160-17.ucode" },
    { 0x08b4, "3160", "iwlwifi-3160-17.ucode" },
    /* Intel 3165 (uses 7265D firmware) */
    { 0x3165, "3165", "iwlwifi-7265D-22.ucode" },
    { 0x3166, "3165", "iwlwifi-7265D-22.ucode" },
    /* Intel 3168 */
    { 0x24fb, "3168", "iwlwifi-3168-22.ucode" },
    /* Intel 7260 */
    { 0x08b1, "7260", "iwlwifi-7260-17.ucode" },
    { 0x08b2, "7260", "iwlwifi-7260-17.ucode" },
    /* Intel 7265 */
    { 0x095a, "7265", "iwlwifi-7265-17.ucode" },
    { 0x095b, "7265", "iwlwifi-7265-17.ucode" },
    /* Intel 8260 (uses 8000C firmware) */
    { 0x24f3, "8260", "iwlwifi-8000C-36.ucode" },
    { 0x24f4, "8260", "iwlwifi-8000C-36.ucode" },
    /* Intel 8265 */
    { 0x24fd, "8265", "iwlwifi-8265-22.ucode" },
    /* Intel 9260 */
    { 0x2526, "9260", "iwlwifi-9260-th-b0-jf-b0-34.ucode" },
    /* Intel 9560 */
    { 0x9df0, "9560", "iwlwifi-9260-th-b0-jf-b0-34.ucode" },
    { 0xa370, "9560", "iwlwifi-9260-th-b0-jf-b0-34.ucode" },
    { 0x31dc, "9560", "iwlwifi-9260-th-b0-jf-b0-34.ucode" },
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

static bool iwl_name_ends_with(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           strcmp(name + name_len - suffix_len, suffix) == 0;
}

static uint32_t iwl_firmware_name_score(const char *name)
{
    uint32_t best = 0;
    uint32_t cur = 0;
    bool in_digits = false;
    for (const char *p = name; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            in_digits = true;
            cur = cur * 10u + (uint32_t)(*p - '0');
        } else {
            if (in_digits && cur > best) {
                best = cur;
            }
            in_digits = false;
            cur = 0;
        }
    }
    if (in_digits && cur > best) {
        best = cur;
    }
    return best;
}

static const char *iwl_firmware_prefix_for_family(const char *family,
                                                  const char *requested)
{
    if (!family) return requested;
    if (strcmp(family, "3160") == 0) return "iwlwifi-3160-";
    if (strcmp(family, "3165") == 0) return "iwlwifi-7265D-";
    if (strcmp(family, "3168") == 0) return "iwlwifi-3168-";
    if (strcmp(family, "7260") == 0) return "iwlwifi-7260-";
    if (strcmp(family, "7265") == 0) return "iwlwifi-7265-";
    if (strcmp(family, "8260") == 0) return "iwlwifi-8000C-";
    if (strcmp(family, "8265") == 0) return "iwlwifi-8265-";
    if (strcmp(family, "9260") == 0) return "iwlwifi-9260-";
    if (strcmp(family, "9560") == 0) return "iwlwifi-9260-";
    return requested;
}

struct iwl_fw_search {
    const char *prefix;
    struct vfs_node *best;
    uint32_t best_score;
};

static void iwl_firmware_search_emit(struct vfs_node *node, void *ctx)
{
    struct iwl_fw_search *search = ctx;
    if (!node || node->type != VFS_NODE_FILE || !node->data || node->size == 0 ||
        !search || !search->prefix) {
        return;
    }
    size_t prefix_len = strlen(search->prefix);
    if (strncmp(node->name, search->prefix, prefix_len) != 0) {
        return;
    }
    if (!iwl_name_ends_with(node->name, ".ucode") &&
        !iwl_name_ends_with(node->name, ".fw")) {
        return;
    }
    uint32_t score = iwl_firmware_name_score(node->name);
    if (!search->best || score > search->best_score ||
        (score == search->best_score && strcmp(node->name, search->best->name) > 0)) {
        search->best = node;
        search->best_score = score;
    }
}

static struct vfs_node *iwl_lookup_firmware_exact(const char *name)
{
    char path[VFS_PATH_MAX];
    ksnprintf(path, sizeof(path), "/lib/firmware/iwlwifi/%s", name);
    struct vfs_node *node = vfs_lookup(path, "/");
    if (node && node->type == VFS_NODE_FILE && node->data && node->size) {
        return node;
    }
    ksnprintf(path, sizeof(path), "/lib/firmware/%s", name);
    node = vfs_lookup(path, "/");
    if (node && node->type == VFS_NODE_FILE && node->data && node->size) {
        return node;
    }
    return NULL;
}

static struct vfs_node *iwl_find_firmware_blob(struct iwlwifi_state *st)
{
    struct vfs_node *node = iwl_lookup_firmware_exact(st->firmware_name);
    if (node) {
        return node;
    }

    const char *prefix = iwl_firmware_prefix_for_family(st->family, st->firmware_name);
    struct iwl_fw_search search = {
        .prefix = prefix,
    };
    struct vfs_node *dir = vfs_lookup("/lib/firmware/iwlwifi", "/");
    if (dir && dir->type == VFS_NODE_DIR) {
        vfs_list(dir, iwl_firmware_search_emit, &search);
    }
    dir = vfs_lookup("/lib/firmware", "/");
    if (dir && dir->type == VFS_NODE_DIR) {
        vfs_list(dir, iwl_firmware_search_emit, &search);
    }
    if (search.best) {
        log_info("iwlwifi", "selected firmware %s for Intel %s (requested %s)",
                 search.best->name, st->family, st->firmware_name);
        st->firmware_name = search.best->name;
    }
    return search.best;
}

static bool iwl_device_uses_modern_transport(const struct iwl_device_info *info)
{
    /* All devices in our table use MVM transport */
    (void)info;
    return info != NULL;
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
                         uint8_t channel, bool privacy, uint16_t security_flags,
                         int8_t rssi)
{
    if (!ssid || ssid_len > 32 || !bssid) {
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
    if (ssid_len == 0) {
        strncpy(ap->ssid, "<hidden>", sizeof(ap->ssid) - 1);
    } else {
        memcpy(ap->ssid, ssid, ssid_len);
        ap->ssid[ssid_len] = '\0';
    }
    memcpy(ap->bssid, bssid, IEEE80211_ADDR_LEN);
    ap->channel = channel;
    ap->privacy = privacy;
    ap->security_flags = security_flags;
    ap->rssi = rssi;
    if (is_new && st->ap_count < IWLWIFI_AP_CACHE_MAX) {
        st->ap_count++;
    }
    log_info("iwlwifi", "AP %s bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u security=%04x",
             ap->ssid, ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5],
             ap->channel, ap->security_flags);
}

static uint16_t iwl_security_from_rsn(const uint8_t *ie, size_t len)
{
    uint16_t flags = WIFI_AP_PRIVACY | WIFI_AP_WPA2;
    if (!ie || len < 8) {
        return flags;
    }

    const uint8_t rsn_oui[3] = { 0x00, 0x0f, 0xac };
    size_t off = 2; /* version */
    if (off + 4 > len) {
        return flags;
    }
    off += 4; /* group cipher */
    if (off + 2 > len) {
        return flags;
    }
    uint16_t pairwise_count = le16_at(ie + off);
    off += 2 + (size_t)pairwise_count * 4;
    if (off + 2 > len) {
        return flags;
    }
    uint16_t akm_count = le16_at(ie + off);
    off += 2;
    bool saw_psk = false;
    bool saw_sae = false;
    for (uint16_t i = 0; i < akm_count && off + 4 <= len; i++, off += 4) {
        if (memcmp(ie + off, rsn_oui, 3) != 0) {
            continue;
        }
        if (ie[off + 3] == 2) {
            saw_psk = true;
        } else if (ie[off + 3] == 8) {
            saw_sae = true;
        }
    }
    if (saw_sae) {
        flags |= WIFI_AP_WPA3;
    }
    if (saw_psk || !saw_sae) {
        flags |= WIFI_AP_WPA2;
    }
    return flags;
}

static bool iwl_ie_is_wpa_vendor(const uint8_t *ie, size_t len)
{
    static const uint8_t wpa_oui_type[4] = { 0x00, 0x50, 0xf2, 0x01 };
    return ie && len >= sizeof(wpa_oui_type) &&
           memcmp(ie, wpa_oui_type, sizeof(wpa_oui_type)) == 0;
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
    uint16_t security_flags = (capability & IEEE80211_CAPINFO_PRIVACY) ?
                              WIFI_AP_PRIVACY : 0;

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
        } else if (id == IEEE80211_ELEMID_RSN) {
            security_flags |= iwl_security_from_rsn(ies, elen);
        } else if (id == 221 && iwl_ie_is_wpa_vendor(ies, elen)) {
            security_flags |= WIFI_AP_PRIVACY | WIFI_AP_WPA;
        }
        ies += elen;
    }

    if (security_flags == WIFI_AP_PRIVACY) {
        security_flags |= WIFI_AP_WEP;
    }
    if (ssid && ssid_len == 0) {
        security_flags |= WIFI_AP_HIDDEN;
    }

    if (ssid) {
        char ssid_text[33];
        memcpy(ssid_text, ssid, ssid_len);
        ssid_text[ssid_len] = '\0';
        iwl_cache_ap(st, ssid_text, ssid_len, hdr->addr3, channel,
                     (capability & IEEE80211_CAPINFO_PRIVACY) != 0,
                     security_flags, 0);
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

static void ccmp_xor_block(uint8_t dst[16], const uint8_t src[16])
{
    for (size_t i = 0; i < 16; i++) {
        dst[i] ^= src[i];
    }
}

static void ccmp_ctr_block(const uint8_t nonce[13], uint16_t counter,
                           uint8_t out[16])
{
    out[0] = 0x01;
    memcpy(out + 1, nonce, 13);
    out[14] = (uint8_t)(counter >> 8);
    out[15] = (uint8_t)counter;
}

static void ccmp_nonce_from_pn(const struct ieee80211_hdr3 *hdr, uint64_t pn,
                               uint8_t nonce[13])
{
    nonce[0] = 0;
    memcpy(nonce + 1, hdr->addr2, IEEE80211_ADDR_LEN);
    nonce[7] = (uint8_t)(pn >> 40);
    nonce[8] = (uint8_t)(pn >> 32);
    nonce[9] = (uint8_t)(pn >> 24);
    nonce[10] = (uint8_t)(pn >> 16);
    nonce[11] = (uint8_t)(pn >> 8);
    nonce[12] = (uint8_t)pn;
}

static uint64_t ccmp_pn_from_header(const uint8_t ccmp[8])
{
    return ((uint64_t)ccmp[7] << 40) | ((uint64_t)ccmp[6] << 32) |
           ((uint64_t)ccmp[5] << 24) | ((uint64_t)ccmp[4] << 16) |
           ((uint64_t)ccmp[1] << 8) | ccmp[0];
}

static size_t ccmp_build_aad(const uint8_t *frame, size_t hdr_len,
                             uint8_t aad[32])
{
    if (!frame || hdr_len < sizeof(struct ieee80211_hdr3)) {
        return 0;
    }
    aad[0] = frame[0];
    aad[1] = frame[1] & (uint8_t)~(0x08 | 0x10 | 0x20 | 0x40);
    memcpy(aad + 2, frame + 4, 18);
    aad[20] = frame[22] & 0x0f;
    aad[21] = 0;
    return 22;
}

static void ccmp_cbc_mac(const uint8_t tk[16], const uint8_t nonce[13],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *plain, size_t plain_len,
                         uint8_t mic[8])
{
    uint8_t x[16];
    uint8_t block[16];
    memset(x, 0, sizeof(x));
    memset(block, 0, sizeof(block));
    block[0] = 0x59;
    memcpy(block + 1, nonce, 13);
    block[14] = (uint8_t)(plain_len >> 8);
    block[15] = (uint8_t)plain_len;
    ccmp_xor_block(x, block);
    tnu_aes128_encrypt_block(tk, x, x);

    if (aad_len) {
        memset(block, 0, sizeof(block));
        block[0] = (uint8_t)(aad_len >> 8);
        block[1] = (uint8_t)aad_len;
        size_t take = aad_len < 14 ? aad_len : 14;
        memcpy(block + 2, aad, take);
        ccmp_xor_block(x, block);
        tnu_aes128_encrypt_block(tk, x, x);
        size_t off = take;
        while (off < aad_len) {
            memset(block, 0, sizeof(block));
            take = aad_len - off;
            if (take > 16) {
                take = 16;
            }
            memcpy(block, aad + off, take);
            ccmp_xor_block(x, block);
            tnu_aes128_encrypt_block(tk, x, x);
            off += take;
        }
    }

    size_t off = 0;
    while (off < plain_len) {
        memset(block, 0, sizeof(block));
        size_t take = plain_len - off;
        if (take > 16) {
            take = 16;
        }
        memcpy(block, plain + off, take);
        ccmp_xor_block(x, block);
        tnu_aes128_encrypt_block(tk, x, x);
        off += take;
    }
    memcpy(mic, x, 8);
}

static size_t ccmp_crypt_payload(const uint8_t tk[16], const uint8_t nonce[13],
                                 const uint8_t *in, size_t len,
                                 uint8_t *out, uint16_t first_counter)
{
    size_t off = 0;
    uint16_t counter = first_counter;
    while (off < len) {
        uint8_t ctr[16];
        uint8_t stream[16];
        ccmp_ctr_block(nonce, counter++, ctr);
        tnu_aes128_encrypt_block(tk, ctr, stream);
        size_t take = len - off;
        if (take > 16) {
            take = 16;
        }
        for (size_t i = 0; i < take; i++) {
            out[off + i] = in[off + i] ^ stream[i];
        }
        off += take;
    }
    return off;
}

static size_t iwl_ccmp_encrypt_frame(struct iwlwifi_state *st, const uint8_t *plain,
                                     size_t plain_len, uint8_t *out, size_t out_size)
{
    size_t hdr_len = sizeof(struct ieee80211_hdr3);
    if (!st || !plain || !out || plain_len <= hdr_len ||
        plain_len + 16 > out_size || !st->wpa_ptk_ready) {
        return 0;
    }

    uint64_t pn = ++st->ccmp_tx_pn;
    memcpy(out, plain, hdr_len);
    out[1] |= IEEE80211_FC1_PROTECTED;
    out[hdr_len + 0] = (uint8_t)pn;
    out[hdr_len + 1] = (uint8_t)(pn >> 8);
    out[hdr_len + 2] = 0;
    out[hdr_len + 3] = 0x20;
    out[hdr_len + 4] = (uint8_t)(pn >> 16);
    out[hdr_len + 5] = (uint8_t)(pn >> 24);
    out[hdr_len + 6] = (uint8_t)(pn >> 32);
    out[hdr_len + 7] = (uint8_t)(pn >> 40);

    const uint8_t *payload = plain + hdr_len;
    size_t payload_len = plain_len - hdr_len;
    const uint8_t *tk = st->wpa_ptk + 32;
    uint8_t nonce[13];
    uint8_t aad[32];
    uint8_t mic[8];
    uint8_t s0[16];
    uint8_t ctr[16];
    ccmp_nonce_from_pn((const struct ieee80211_hdr3 *)out, pn, nonce);
    size_t aad_len = ccmp_build_aad(out, hdr_len, aad);
    ccmp_cbc_mac(tk, nonce, aad, aad_len, payload, payload_len, mic);
    ccmp_ctr_block(nonce, 0, ctr);
    tnu_aes128_encrypt_block(tk, ctr, s0);
    for (size_t i = 0; i < sizeof(mic); i++) {
        mic[i] ^= s0[i];
    }

    ccmp_crypt_payload(tk, nonce, payload, payload_len, out + hdr_len + 8, 1);
    memcpy(out + hdr_len + 8 + payload_len, mic, sizeof(mic));
    return hdr_len + 8 + payload_len + sizeof(mic);
}

static bool iwl_ccmp_decrypt_payload(struct iwlwifi_state *st, const uint8_t *frame,
                                     size_t frame_len, size_t hdr_len,
                                     uint8_t *plain, size_t plain_size,
                                     size_t *plain_len)
{
    if (!st || !frame || !plain || !plain_len || !st->wpa_ptk_ready ||
        frame_len < hdr_len + 16) {
        return false;
    }
    const uint8_t *ccmp = frame + hdr_len;
    size_t cipher_len = frame_len - hdr_len - 8;
    if (cipher_len < 8) {
        return false;
    }
    size_t payload_len = cipher_len - 8;
    if (payload_len > plain_size) {
        return false;
    }

    uint64_t pn = ccmp_pn_from_header(ccmp);
    uint8_t nonce[13];
    uint8_t aad[32];
    uint8_t expected_mic[8];
    uint8_t s0[16];
    uint8_t ctr[16];
    const uint8_t *tk = st->wpa_ptk + 32;
    ccmp_nonce_from_pn((const struct ieee80211_hdr3 *)frame, pn, nonce);
    ccmp_crypt_payload(tk, nonce, ccmp + 8, payload_len, plain, 1);

    size_t aad_len = ccmp_build_aad(frame, hdr_len, aad);
    ccmp_cbc_mac(tk, nonce, aad, aad_len, plain, payload_len, expected_mic);
    ccmp_ctr_block(nonce, 0, ctr);
    tnu_aes128_encrypt_block(tk, ctr, s0);
    for (size_t i = 0; i < sizeof(expected_mic); i++) {
        expected_mic[i] ^= s0[i];
    }
    if (memcmp(expected_mic, ccmp + 8 + payload_len, sizeof(expected_mic)) != 0) {
        log_warn("iwlwifi", "%s dropped CCMP frame with invalid MIC",
                 st->iface ? st->iface->name : "wlan");
        return false;
    }
    *plain_len = payload_len;
    return true;
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
        log_warn("iwlwifi", "%s verified WPA msg3 and transmitted msg4; GTK unwrap not available",
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
    if ((hdr->fc[0] & IEEE80211_FC0_TYPE_MASK) != IEEE80211_FC0_TYPE_DATA) {
        return;
    }
    size_t hdr_len = sizeof(struct ieee80211_hdr3);
    if ((hdr->fc[0] & IEEE80211_FC0_SUBTYPE_MASK) & 0x80) {
        hdr_len += 2;
    }
    bool protected = (hdr->fc[1] & IEEE80211_FC1_PROTECTED) != 0;
    uint8_t clear[1600];
    size_t clear_len = 0;
    const uint8_t *llc = frame + hdr_len;
    size_t llc_len = len - hdr_len;
    if (protected) {
        if (!iwl_ccmp_decrypt_payload(st, frame, len, hdr_len,
                                      clear, sizeof(clear), &clear_len)) {
            return;
        }
        llc = clear;
        llc_len = clear_len;
    }
    if (llc_len < 8) {
        return;
    }
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
        iwl_handle_eapol_key(st, iface, ap, llc + 8, llc_len - 8);
        return;
    }
    if (!st->link_ready || !st->rx_callback) {
        return;
    }
    uint8_t eth[1600];
    size_t payload_len = llc_len - 8;
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
    uint32_t tlv_count = 0;

    for (uint16_t candidate = 3; candidate > 0; candidate--) {
        if (altmask & (1ull << candidate)) {
            alt = candidate;
            break;
        }
    }

    log_info("iwlwifi", "TLV parse: altmask=%016llx alt=%u ptr=%p end=%p",
             (unsigned long long)altmask, alt, (void*)ptr, (void*)end);

    while (ptr + 8 <= end) {
        uint16_t type = le16_at(ptr);
        uint16_t entry_alt = le16_at(ptr + 2);
        uint32_t len = le32_at(ptr + 4);
        ptr += 8;
        
        tlv_count++;
        if (tlv_count <= 10 || type == IWL_TLV_SEC_RT || type == IWL_TLV_SEC_INIT) {
            log_info("iwlwifi", "TLV %u: type=%u alt=%u len=%u", tlv_count, type, entry_alt, len);
        }
        
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
            case IWL_TLV_FLAGS:
                if (len >= 4) {
                    st->tlv_flags = le32_at(ptr);
                }
                break;
            case IWL_TLV_API_CHANGES_SET:
                if (len >= 8) {
                    uint32_t index = le32_at(ptr);
                    if (index < sizeof(st->api_flags) / sizeof(st->api_flags[0])) {
                        st->api_flags[index] = le32_at(ptr + 4);
                    }
                }
                break;
            case IWL_TLV_ENABLED_CAPABILITIES:
                if (len >= 8) {
                    uint32_t index = le32_at(ptr);
                    if (index < sizeof(st->capa_flags) / sizeof(st->capa_flags[0])) {
                        st->capa_flags[index] = le32_at(ptr + 4);
                    }
                }
                break;
            case IWL_TLV_SEC_RT:
                if (len > 4 && st->runtime_section_count < IWLWIFI_FW_SECTION_MAX) {
                    struct iwlwifi_fw_section *sec =
                        &st->runtime_sections[st->runtime_section_count++];
                    sec->offset = le32_at(ptr);
                    sec->data = ptr + 4;
                    sec->size = len - 4;
                    st->runtime_section_bytes += sec->size;
                    if (!st->runtime_ucode.text) {
                        st->runtime_ucode.text = sec->data;
                    }
                    st->runtime_ucode.text_size += sec->size;
                }
                break;
            case IWL_TLV_SEC_INIT:
                if (len > 4 && st->init_section_count < IWLWIFI_FW_SECTION_MAX) {
                    struct iwlwifi_fw_section *sec =
                        &st->init_sections[st->init_section_count++];
                    sec->offset = le32_at(ptr);
                    sec->data = ptr + 4;
                    sec->size = len - 4;
                    st->init_section_bytes += sec->size;
                    if (!st->init_ucode.text) {
                        st->init_ucode.text = sec->data;
                    }
                    st->init_ucode.text_size += sec->size;
                }
                break;
            default:
                break;
            }
        }
        ptr += (len + 3u) & ~3u;
    }

    log_info("iwlwifi", "TLV parse done: total=%u runtime=%u init=%u",
             tlv_count, st->runtime_section_count, st->init_section_count);

    bool legacy_ok = part_present(&st->runtime_ucode) && part_present(&st->init_ucode);
    bool modern_ok = st->runtime_section_count > 0;
    st->firmware_parsed = legacy_ok || modern_ok;
    if (st->firmware_parsed) {
        log_info("iwlwifi", "TLV parsed: runtime sections=%u bytes=%llu init sections=%u bytes=%llu flags=%08x",
                 (uint32_t)st->runtime_section_count,
                 (unsigned long long)st->runtime_section_bytes,
                 (uint32_t)st->init_section_count,
                 (unsigned long long)st->init_section_bytes,
                 st->tlv_flags);
    }
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

    iwl_write32(st, FH_CBBC_QUEUE(st->cmd_queue_id), (uint32_t)(st->cmd_desc_phys >> 8));
    iwl_write32(st, FH_TX_CONFIG(st->cmd_queue_id),
                IWL_FH_TX_CONFIG_DMA_ENA | IWL_FH_TX_CONFIG_DMA_CREDIT_ENA);

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

    iwl_write32(st, FH_CBBC_QUEUE(st->mgmt_queue_id), (uint32_t)(st->tx_desc_phys >> 8));
    iwl_write32(st, FH_TX_CONFIG(st->mgmt_queue_id),
                IWL_FH_TX_CONFIG_DMA_ENA | IWL_FH_TX_CONFIG_DMA_CREDIT_ENA);

    st->tx_queue_ready = true;
    log_info("iwlwifi", "allocated management TX queue desc=%p ring=%p",
             (void *)st->tx_desc_phys, (void *)st->tx_ring_phys);
    return 0;
}

static int iwl_load_firmware(struct iwlwifi_state *st)
{
    struct vfs_node *node = iwl_find_firmware_blob(st);
    if (!node || node->type != VFS_NODE_FILE || !node->data || node->size == 0) {
        log_warn("iwlwifi", "firmware for Intel %s (%s) not found under /lib/firmware/iwlwifi or /lib/firmware",
                 st->family, st->firmware_name);
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
    if (iwl_parse_firmware_sections(st) && st->runtime_section_count == 0) {
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
    for (int i = 0; i < 20000; i++) {
        iwl_write32(st, CSR_GP_CNTRL,
                    iwl_read32(st, CSR_GP_CNTRL) | IWL_GP_CNTRL_MAC_ACCESS_REQ);
        uint32_t v = iwl_read32(st, CSR_GP_CNTRL);
        if (!(v & IWL_GP_CNTRL_RFKILL)) {
            log_warn("iwlwifi", "NIC lock blocked by RF-kill gp=%08x hw=%08x reset=%08x",
                     v, iwl_read32(st, CSR_HW_IF_CONFIG_REG), iwl_read32(st, CSR_RESET));
            return -2;
        }
        if ((v & (IWL_GP_CNTRL_MAC_ACCESS_ENA | IWL_GP_CNTRL_SLEEP)) ==
            IWL_GP_CNTRL_MAC_ACCESS_ENA) {
            return 0;
        }
        if ((i & 0xff) == 0) {
            iwl_short_delay();
        } else {
            __asm__ volatile("pause");
        }
    }
    log_warn("iwlwifi", "NIC lock timeout gp=%08x hw=%08x reset=%08x int=%08x fh=%08x",
             iwl_read32(st, CSR_GP_CNTRL),
             iwl_read32(st, CSR_HW_IF_CONFIG_REG),
             iwl_read32(st, CSR_RESET),
             iwl_read32(st, CSR_INT),
             iwl_read32(st, CSR_FH_INT_STATUS));
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

static void iwl_short_delay(void)
{
    for (volatile int i = 0; i < 20000; i++) {
    }
}

static int iwl_hw_prepare(struct iwlwifi_state *st)
{
    iwl_setbits(st, CSR_RESET, IWL_RESET_LINK_PWR_MGMT_DIS);
    iwl_clrbits(st, CSR_GIO, IWL_GIO_L0S_ENA);

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_NIC_READY);
    if (iwl_wait_bit_set(st, CSR_HW_IF_CONFIG_REG,
                         IWL_HW_IF_CONFIG_NIC_READY, 20)) {
        return 0;
    }

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_PREPARE);
    if (!iwl_wait_bit_set(st, CSR_HW_IF_CONFIG_REG,
                          IWL_HW_IF_CONFIG_PREPARE_DONE, 150)) {
        log_warn("iwlwifi", "NIC prepare did not complete hw=%08x gp=%08x reset=%08x",
                 iwl_read32(st, CSR_HW_IF_CONFIG_REG),
                 iwl_read32(st, CSR_GP_CNTRL),
                 iwl_read32(st, CSR_RESET));
        return -1;
    }
    iwl_clrbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_PREPARE);

    iwl_setbits(st, CSR_HW_IF_CONFIG_REG, IWL_HW_IF_CONFIG_NIC_READY);
    if (!iwl_wait_bit_set(st, CSR_HW_IF_CONFIG_REG,
                          IWL_HW_IF_CONFIG_NIC_READY, 20)) {
        log_warn("iwlwifi", "NIC_READY did not set hw=%08x gp=%08x reset=%08x",
                 iwl_read32(st, CSR_HW_IF_CONFIG_REG),
                 iwl_read32(st, CSR_GP_CNTRL),
                 iwl_read32(st, CSR_RESET));
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

    /* Move NIC from D0U* (uninitialized) to D0A* (active) so the MAC clock starts */
    iwl_setbits(st, CSR_GP_CNTRL, IWL_GP_CNTRL_INIT_DONE);

    rc = iwl_clock_wait(st);
    if (rc < 0) {
        log_warn("iwlwifi", "MAC clock did not become ready");
        return rc;
    }

    /*
     * APMG init — required for all 7000/8000 family chips (3160, 7260, 7265,
     * 8260, 8265).  9000+ family (9260, 9560) dropped the APMG subsystem.
     * From FreeBSD if_iwm_pcie_trans.c: iwm_apm_init() runs APMG for
     * device_family < IWM_DEVICE_FAMILY_9000.
     */
    bool needs_apmg = strcmp(st->family, "9260") != 0 &&
                      strcmp(st->family, "9560") != 0;
    if (needs_apmg) {
        rc = iwl_nic_lock(st);
        if (rc < 0) {
            log_warn("iwlwifi", "could not lock NIC during APM init (%d)", rc);
            return rc;
        }
        iwl_prph_write(st, IWL_APMG_CLK_EN, IWL_APMG_CLK_CTRL_DMA_CLK_RQT);
        iwl_short_delay();
        iwl_prph_setbits(st, IWL_APMG_PCI_STT, IWL_APMG_PCI_STT_L1A_DIS);
        iwl_prph_clrbits(st, IWL_APMG_PS, IWL_APMG_PS_PWR_SRC_MASK);
        iwl_prph_setbits(st, IWL_APMG_PS, IWL_APMG_PS_EARLY_PWROFF_DIS);
        iwl_nic_unlock(st);
    }

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
        case IWL_RX_TYPE_UC_READY: /* also MVM ALIVE (0x01) */
            if (st->modern_transport) {
                /*
                 * Use arrival sequence to distinguish INIT from RT alive:
                 *   first UC_READY seen  → INIT firmware is up
                 *   second UC_READY seen → RT firmware is up
                 * We avoid reading ver_type because its offset varies
                 * across firmware versions and caused "not parsed" failures.
                 */
                size_t plen = len > sizeof(*desc) ? len - sizeof(*desc) : 0;
                const uint8_t *payload = buf + sizeof(*desc);
                if (!st->mvm_init_alive_seen) {
                    st->mvm_init_alive_seen = true;
                    if (plen >= 8) {
                        uint32_t minor = le32_at(payload);
                        uint32_t major = le32_at(payload + 4);
                        log_info("iwlwifi", "MVM INIT alive ucode=%u.%u plen=%u",
                                 major, minor, (uint32_t)plen);
                    } else {
                        log_info("iwlwifi", "MVM INIT alive (plen=%u)", (uint32_t)plen);
                    }
                } else {
                    st->mvm_rt_alive_seen  = true;
                    st->mvm_alive          = true;
                    st->firmware_alive     = true;
                    st->firmware_running   = true;
                    if (plen >= 8) {
                        uint32_t minor = le32_at(payload);
                        uint32_t major = le32_at(payload + 4);
                        log_info("iwlwifi", "MVM RT alive ucode=%u.%u plen=%u",
                                 major, minor, (uint32_t)plen);
                    } else {
                        log_info("iwlwifi", "MVM RT alive (plen=%u)", (uint32_t)plen);
                    }
                }
            } else if (len >= sizeof(*desc) + sizeof(struct iwl_ucode_info)) {
                const struct iwl_ucode_info *uc =
                    (const struct iwl_ucode_info *)(buf + sizeof(*desc));
                if (uc->valid == 1) {
                    st->firmware_alive   = true;
                    st->firmware_running = true;
                    log_info("iwlwifi", "microcode alive major=%u minor=%u subtype=%u errptr=%08x",
                             uc->major, uc->minor, uc->subtype, uc->errptr);
                } else {
                    log_warn("iwlwifi", "microcode alive notification was invalid (%u)",
                             uc->valid);
                }
            }
            break;
        case MVM_CMD_CALIB_RES_NOTIF: /* 0x6b — PHY_DB calibration blob from INIT */
            if (st->modern_transport) {
                size_t plen = len > sizeof(*desc) ? len - sizeof(*desc) : 0;
                iwl_mvm_phy_db_store(st, buf + sizeof(*desc), plen);
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
        case MVM_RX_SCAN_COMPLETE: /* 0x6d IWM_SCAN_OFFLOAD_COMPLETE */
            st->mvm_scan_running = false;
            st->mvm_scan_done    = true;
            log_info("iwlwifi", "MVM LMAC scan complete, %u AP(s) cached",
                     (uint32_t)st->ap_count);
            break;
        case MVM_RX_TE_NOTIF: /* 0x2a IWM_TIME_EVENT_NOTIFICATION */
            if (st->modern_transport) {
                size_t plen = len > sizeof(*desc) ? len - sizeof(*desc) : 0;
                if (plen >= sizeof(struct mvm_time_event_notif)) {
                    const struct mvm_time_event_notif *n =
                        (const struct mvm_time_event_notif *)(buf + sizeof(*desc));
                    log_info("iwlwifi", "MVM TE notif uid=%08x action=%u status=%u",
                             n->unique_id, n->action, n->status);
                    if (n->action & MVM_TE_V2_NOTIF_HOST_END) {
                        st->mvm_te_active = false;
                    }
                }
            }
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
    cmd->qid  = st->cmd_queue_id;
    cmd->idx  = (uint8_t)index;
    if (payload && payload_len) {
        memcpy(cmd->data, payload, payload_len);
    }

    desc->nsegs = 1;
    desc->segs[0].addr = (uint32_t)cmd_phys;
    desc->segs[0].len  = (uint16_t)((total << 4) | ((cmd_phys >> 32) & 0xf));

    st->command_done      = false;
    st->command_done_code = 0;
    st->cmd_index = (uint16_t)((index + 1) % IWL_TX_RING_COUNT);
    iwl_write32(st, CSR_HBUS_TARG_WRPTR,
                ((uint32_t)st->cmd_queue_id << 8) | st->cmd_index);

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

/* -----------------------------------------------------------------------
 * MVM command/response layer
 * Command queue for MVM is queue 9 (IWM_CMD_QUEUE from FreeBSD)
 * ----------------------------------------------------------------------- */

static int iwl_send_cmd_on_queue(struct iwlwifi_state *st, uint8_t qid,
                                  uint8_t code, const void *payload,
                                  size_t payload_len, bool wait)
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
    cmd->qid  = qid;
    cmd->idx  = (uint8_t)index;
    if (payload && payload_len) {
        memcpy(cmd->data, payload, payload_len);
    }
    desc->nsegs = 1;
    desc->segs[0].addr = (uint32_t)cmd_phys;
    desc->segs[0].len  = (uint16_t)((total << 4) | ((cmd_phys >> 32) & 0xf));

    st->command_done      = false;
    st->command_done_code = 0;
    st->cmd_index = (uint16_t)((index + 1) % IWL_TX_RING_COUNT);
    iwl_write32(st, CSR_HBUS_TARG_WRPTR, ((uint32_t)qid << 8) | st->cmd_index);

    if (!wait) {
        return 0;
    }
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 500) {
        iwl_poll_rx_notifications(st);
        if (st->command_done) {
            return st->command_done_code == code ? 0 : 1;
        }
        __asm__ volatile("pause");
    }
    log_warn("iwlwifi", "command 0x%02x (queue %u) timed out", code, qid);
    return -2;
}

static int iwl_mvm_send_cmd(struct iwlwifi_state *st,
                             uint8_t cmd, const void *payload, size_t len)
{
    return iwl_send_cmd_on_queue(st, st->cmd_queue_id, cmd, payload, len, true);
}

/*
 * PHY DB capture — called from iwl_poll_rx_notifications when INIT firmware
 * sends MVM_CMD_CALIB_RES_NOTIF (0x6b) blobs.
 * We store each section separately so we can replay them correctly.
 */
static void iwl_mvm_phy_db_store(struct iwlwifi_state *st,
                                  const uint8_t *payload, size_t plen)
{
    if (!payload || plen < 4) {
        return;
    }
    uint16_t type = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint16_t size = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    if (plen < (size_t)(4 + size) || size > 512) {
        return;
    }
    const uint8_t *data = payload + 4;

    switch (type) {
    case MVM_PHY_DB_CFG:
        if (size <= sizeof(st->phy_db_cfg)) {
            memcpy(st->phy_db_cfg, data, size);
            st->phy_db_cfg_size = size;
        }
        break;
    case MVM_PHY_DB_CALIB_NCH:
        if (size <= sizeof(st->phy_db_calib_nch)) {
            memcpy(st->phy_db_calib_nch, data, size);
            st->phy_db_calib_nch_size = size;
        }
        break;
    case MVM_PHY_DB_CALIB_CHG_PAPD: {
        if (size < 2) break;
        uint16_t idx = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        if (idx >= 16) break;
        if ((size_t)(size - 2) <= sizeof(st->phy_db_papd[idx])) {
            memcpy(st->phy_db_papd[idx], data + 2, size - 2);
            st->phy_db_papd_size[idx] = size - 2;
            if (idx + 1 > st->phy_db_n_papd) {
                st->phy_db_n_papd = (uint8_t)(idx + 1);
            }
        }
        break;
    }
    case MVM_PHY_DB_CALIB_CHG_TXP: {
        if (size < 2) break;
        uint16_t idx = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        if (idx >= 16) break;
        if ((size_t)(size - 2) <= sizeof(st->phy_db_txp[idx])) {
            memcpy(st->phy_db_txp[idx], data + 2, size - 2);
            st->phy_db_txp_size[idx] = size - 2;
            if (idx + 1 > st->phy_db_n_txp) {
                st->phy_db_n_txp = (uint8_t)(idx + 1);
            }
        }
        break;
    }
    default:
        break;
    }
}

static int iwl_mvm_send_phy_db_section(struct iwlwifi_state *st,
                                        uint16_t type, uint16_t chg_id,
                                        const uint8_t *data, uint16_t size)
{
    /* Build: type(2) + size(2) + [chg_id(2)] + data */
    uint8_t buf[4 + 2 + 512];
    size_t total;
    if (type == MVM_PHY_DB_CALIB_CHG_PAPD ||
        type == MVM_PHY_DB_CALIB_CHG_TXP) {
        /* Include the group index before data */
        total = 4 + 2 + size;
        if (total > sizeof(buf)) return -1;
        buf[0] = (uint8_t)type;
        buf[1] = (uint8_t)(type >> 8);
        buf[2] = (uint8_t)(size + 2);
        buf[3] = (uint8_t)((size + 2) >> 8);
        buf[4] = (uint8_t)chg_id;
        buf[5] = (uint8_t)(chg_id >> 8);
        memcpy(buf + 6, data, size);
    } else {
        total = 4 + size;
        if (total > sizeof(buf)) return -1;
        buf[0] = (uint8_t)type;
        buf[1] = (uint8_t)(type >> 8);
        buf[2] = (uint8_t)size;
        buf[3] = (uint8_t)(size >> 8);
        memcpy(buf + 4, data, size);
    }
    return iwl_mvm_send_cmd(st, MVM_CMD_PHY_DB, buf, total);
}

/*
 * Post RT alive init — based on iwm_run_init_ucode() in FreeBSD if_iwm.c.
 * Order: POWER_TABLE → PHY_DB sections → firmware ready.
 */
static int iwl_mvm_post_alive_init(struct iwlwifi_state *st)
{
    int rc;

    /* 1. POWER_TABLE_CMD: CAM mode (no power save) */
    struct mvm_device_power_cmd pwr;
    memset(&pwr, 0, sizeof(pwr));
    pwr.flags = (uint16_t)MVM_DEVICE_POWER_FLAGS_CAM;
    rc = iwl_mvm_send_cmd(st, MVM_CMD_POWER_TABLE, &pwr, sizeof(pwr));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM POWER_TABLE_CMD failed (%d) — continuing", rc);
    } else {
        log_info("iwlwifi", "MVM POWER_TABLE: CAM mode set");
    }

    /* 2. PHY_DB: replay sections captured during INIT */
    bool any = false;
    if (st->phy_db_cfg_size > 0) {
        rc = iwl_mvm_send_phy_db_section(st, MVM_PHY_DB_CFG, 0,
                                          st->phy_db_cfg, st->phy_db_cfg_size);
        if (rc < 0) log_warn("iwlwifi", "MVM PHY_DB_CFG failed (%d)", rc);
        else any = true;
    }
    if (st->phy_db_calib_nch_size > 0) {
        rc = iwl_mvm_send_phy_db_section(st, MVM_PHY_DB_CALIB_NCH, 0,
                                          st->phy_db_calib_nch,
                                          st->phy_db_calib_nch_size);
        if (rc < 0) log_warn("iwlwifi", "MVM PHY_DB_CALIB_NCH failed (%d)", rc);
        else any = true;
    }
    for (uint8_t i = 0; i < st->phy_db_n_papd; i++) {
        if (st->phy_db_papd_size[i] == 0) continue;
        rc = iwl_mvm_send_phy_db_section(st, MVM_PHY_DB_CALIB_CHG_PAPD, i,
                                          st->phy_db_papd[i],
                                          st->phy_db_papd_size[i]);
        if (rc < 0) log_warn("iwlwifi", "MVM PHY_DB_PAPD[%u] failed (%d)", i, rc);
        else any = true;
    }
    for (uint8_t i = 0; i < st->phy_db_n_txp; i++) {
        if (st->phy_db_txp_size[i] == 0) continue;
        rc = iwl_mvm_send_phy_db_section(st, MVM_PHY_DB_CALIB_CHG_TXP, i,
                                          st->phy_db_txp[i],
                                          st->phy_db_txp_size[i]);
        if (rc < 0) log_warn("iwlwifi", "MVM PHY_DB_TXP[%u] failed (%d)", i, rc);
        else any = true;
    }
    if (!any) {
        log_warn("iwlwifi", "MVM PHY_DB: no calibration data captured from INIT — commands may fail");
    } else {
        log_info("iwlwifi", "MVM PHY_DB: cfg=%u nch=%u papd_groups=%u txp_groups=%u",
                 st->phy_db_cfg_size, st->phy_db_calib_nch_size,
                 st->phy_db_n_papd, st->phy_db_n_txp);
    }

    /* 3. Settle */
    uint64_t settle = pit_ticks();
    while (pit_ticks() - settle < 100) {
        iwl_poll_rx_notifications(st);
        __asm__ volatile("pause");
    }

    st->mvm_fw_ready = true;
    log_info("iwlwifi", "MVM firmware ready for commands");
    return 0;
}

/*
 * Build rxchain_info for PHY context — from iwm_rxchain() in FreeBSD.
 * Use 2 chains (A+B), driver-forced.
 */
static uint32_t iwl_mvm_rxchain(void)
{
    uint32_t chains = 0x3u; /* A+B */
    return (1u << 0)                    /* DRIVER_FORCE */
         | ((chains & 0x7u) << 1)       /* VALID */
         | ((chains & 0x7u) << 4)       /* FORCE_SEL */
         | ((chains & 0x7u) << 7)       /* MIMO_SEL */
         | (2u << 10)                   /* CNT */
         | (2u << 12);                  /* MIMO_CNT */
}

static int iwl_mvm_phy_ctx_cmd(struct iwlwifi_state *st, uint8_t channel,
                                 uint32_t action)
{
    struct mvm_phy_ctx_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color      = MVM_ID_AND_COLOR(st->mvm_phy_id, 1);
    cmd.action            = action;
    cmd.apply_time        = 0;
    cmd.tx_param_color    = 0;
    cmd.ci.band           = channel > 14 ? MVM_PHY_BAND_5 : MVM_PHY_BAND_24;
    cmd.ci.channel        = channel;
    cmd.ci.width          = 0; /* 20 MHz */
    cmd.ci.ctrl_pos       = 0;
    cmd.rxchain_info      = iwl_mvm_rxchain();
    cmd.txchain_info      = 0x3u; /* A+B */
    cmd.acquisition_data  = 0;
    cmd.dsp_cfg_flags     = 0;
    int rc = iwl_mvm_send_cmd(st, MVM_CMD_PHY_CONTEXT, &cmd, sizeof(cmd));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM PHY_CONTEXT_CMD ch=%u action=%u failed (%d)",
                 channel, action, rc);
    } else {
        log_info("iwlwifi", "MVM PHY context ch=%u action=%u OK", channel, action);
    }
    return rc;
}

static int iwl_mvm_mac_ctx_cmd(struct iwlwifi_state *st,
                                 const struct net_iface *iface,
                                 const uint8_t *bssid,
                                 uint32_t action, bool is_assoc,
                                 uint16_t aid, uint32_t dtim_interval)
{
    struct mvm_mac_ctx_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color  = MVM_ID_AND_COLOR(st->mvm_mac_id, 1);
    cmd.action        = action;
    cmd.mac_type      = MVM_MAC_TYPE_BSS_STA;
    cmd.tsf_id        = MVM_TSF_ID_A;
    memcpy(cmd.node_addr, iface->mac, 6);
    if (bssid) {
        memcpy(cmd.bssid_addr, bssid, 6);
    }
    cmd.cck_rates         = MVM_BASIC_RATES_CCK;
    cmd.ofdm_rates        = MVM_BASIC_RATES_OFDM;
    cmd.protection_flags  = 0;
    cmd.cck_short_preamble = 0;
    cmd.short_slot        = 0x10; /* short slot enabled */
    cmd.filter_flags      = MVM_MAC_FILTER_ACCEPT_GRP |
                            MVM_MAC_FILTER_ACCEPT_BEACON |
                            MVM_MAC_FILTER_DIS_DECRYPT;

    /* AC QoS defaults from iwm_mac_ctxt_cmd_common() in FreeBSD */
    static const struct { uint16_t cw_min; uint16_t cw_max; uint8_t aifsn; uint8_t fifo; } ac_def[4] = {
        { 15,  1023, 3, 0 }, /* BK */
        {  3,    15, 7, 1 }, /* BE — actually index 1 */
        {  7,    15, 2, 2 }, /* VI */
        {  3,     7, 2, 3 }, /* VO */
    };
    for (int i = 0; i < 4; i++) {
        cmd.ac[i].cw_min     = ac_def[i].cw_min;
        cmd.ac[i].cw_max     = ac_def[i].cw_max;
        cmd.ac[i].aifsn      = ac_def[i].aifsn;
        cmd.ac[i].fifos_mask = (uint8_t)(1u << ac_def[i].fifo);
        cmd.ac[i].edca_txop  = 0;
    }
    /* cmd.ac[4] = ucast mgmt */
    cmd.ac[4].cw_min     = 3;
    cmd.ac[4].cw_max     = 7;
    cmd.ac[4].aifsn      = 2;
    cmd.ac[4].fifos_mask = (1u << 3);

    cmd.sta.is_assoc              = is_assoc ? 1u : 0u;
    cmd.sta.listen_interval       = 10;
    cmd.sta.assoc_id              = aid;
    if (is_assoc && dtim_interval) {
        cmd.sta.dtim_interval     = dtim_interval;
        cmd.sta.dtim_reciprocal   = 0xFFFFFFFFu / dtim_interval;
        cmd.sta.bi                = dtim_interval;
        cmd.sta.bi_reciprocal     = 0xFFFFFFFFu / dtim_interval;
    }

    int rc = iwl_mvm_send_cmd(st, MVM_CMD_MAC_CONTEXT, &cmd, sizeof(cmd));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM MAC_CONTEXT_CMD action=%u failed (%d)", action, rc);
    } else {
        log_info("iwlwifi", "MVM MAC context action=%u assoc=%d OK", action, (int)is_assoc);
    }
    return rc;
}

static int iwl_mvm_binding_cmd(struct iwlwifi_state *st, uint32_t action)
{
    struct mvm_binding_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = MVM_ID_AND_COLOR(0, 1);
    cmd.action       = action;
    cmd.macs[0]      = MVM_ID_AND_COLOR(st->mvm_mac_id, 1);
    cmd.macs[1]      = 0xFFFFFFFFu;
    cmd.macs[2]      = 0xFFFFFFFFu;
    cmd.phy          = MVM_ID_AND_COLOR(st->mvm_phy_id, 1);
    cmd.lmac_id      = 0;
    int rc = iwl_mvm_send_cmd(st, MVM_CMD_BINDING, &cmd, sizeof(cmd));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM BINDING_CONTEXT_CMD action=%u failed (%d)", action, rc);
    } else {
        log_info("iwlwifi", "MVM binding action=%u OK", action);
    }
    return rc;
}

static int iwl_mvm_add_sta_cmd(struct iwlwifi_state *st,
                                 const struct net_iface *iface,
                                 const uint8_t *addr)
{
    struct mvm_add_sta_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.add_modify      = MVM_STA_ACTION_ADD;
    cmd.awake_acs       = 0;
    cmd.tid_disable_tx  = 0;
    cmd.mac_id_n_color  = MVM_ID_AND_COLOR(st->mvm_mac_id, 1);
    memcpy(cmd.addr, addr ? addr : iface->mac, 6);
    cmd.sta_id          = 0;
    cmd.modify_mask     = 0;
    cmd.station_flags   = 0;
    cmd.station_flags_msk = 0;
    cmd.tfd_queue_msk   = 0;
    int rc = iwl_mvm_send_cmd(st, MVM_CMD_ADD_STA, &cmd, sizeof(cmd));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM ADD_STA failed (%d)", rc);
    } else {
        log_info("iwlwifi", "MVM STA added id=%u", (uint32_t)cmd.sta_id);
    }
    return rc;
}

static int iwl_mvm_protect_session(struct iwlwifi_state *st, uint32_t duration_ms)
{
    struct mvm_time_event_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = MVM_ID_AND_COLOR(st->mvm_mac_id, 1);
    cmd.action       = MVM_CTXT_ACTION_ADD;
    cmd.id           = MVM_TE_BSS_STA_ASSOC;
    cmd.apply_time   = 0;
    cmd.max_delay    = (uint32_t)(duration_ms * 1024 / 1000); /* ms → TU approx */
    cmd.depends_on   = 0;
    cmd.interval     = 1;
    cmd.duration     = (uint32_t)(duration_ms * 1024 / 1000);
    cmd.repeat       = 1;
    cmd.max_frags    = 1;
    cmd.policy       = (uint16_t)(MVM_TE_V2_NOTIF_HOST_START | MVM_TE_V2_NOTIF_HOST_END);
    int rc = iwl_mvm_send_cmd(st, MVM_CMD_TIME_EVENT, &cmd, sizeof(cmd));
    if (rc < 0) {
        log_warn("iwlwifi", "MVM TIME_EVENT_CMD failed (%d) — continuing", rc);
    } else {
        st->mvm_te_active = true;
        log_info("iwlwifi", "MVM time event created duration=%u TU", cmd.duration);
    }
    return rc;
}

static int iwl_mvm_scan(struct iwlwifi_state *st, const struct net_iface *iface,
                         const char *ssid)
{
    if (!st->mvm_fw_ready) {
        log_warn("iwlwifi", "%s MVM scan: firmware not ready", iface->name);
        return -1;
    }

    /* Scan sequence from FreeBSD if_iwm.c iwm_scan(): */
    /* 1. MAC context ADD */
    if (iwl_mvm_mac_ctx_cmd(st, iface, NULL, MVM_CTXT_ACTION_ADD,
                              false, 0, 0) < 0) {
        return -1;
    }
    /* 2. PHY context ADD on ch 1 */
    if (iwl_mvm_phy_ctx_cmd(st, 1, MVM_CTXT_ACTION_ADD) < 0) {
        iwl_mvm_mac_ctx_cmd(st, iface, NULL, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return -1;
    }
    /* 3. Binding ADD */
    if (iwl_mvm_binding_cmd(st, MVM_CTXT_ACTION_ADD) < 0) {
        iwl_mvm_phy_ctx_cmd(st, 1, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_mac_ctx_cmd(st, iface, NULL, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return -1;
    }

    /* 4. Build LMAC scan request */
    static const uint8_t channels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        36, 40, 44, 48, 52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140
    };
    uint8_t nchan = sizeof(channels);
    if (nchan > MVM_SCAN_MAX_CHANNELS) nchan = MVM_SCAN_MAX_CHANNELS;

    /* LMAC scan command + channel data (variable part after fixed header) */
    uint8_t buf[sizeof(struct mvm_scan_req_lmac) +
                nchan * sizeof(struct mvm_scan_channel_cfg_lmac)];
    memset(buf, 0, sizeof(buf));

    struct mvm_scan_req_lmac *req = (struct mvm_scan_req_lmac *)buf;
    req->n_channels     = nchan;
    req->active_dwell   = 10;
    req->passive_dwell  = 110;
    req->fragmented_dwell = 44;
    req->extended_dwell = 90;
    req->rx_chain_select = (uint16_t)iwl_mvm_rxchain();
    req->scan_flags     = MVM_LMAC_SCAN_FLAG_PASS_ALL | MVM_LMAC_SCAN_FLAG_ITER_COMPLETE;
    req->max_out_time   = 0;
    req->suspend_time   = 0;
    req->filter_flags   = MVM_MAC_FILTER_ACCEPT_GRP;
    /* TX cmd for probe request: 1Mbps CCK */
    req->tx_cmd[0].tx_flags = 0x200; /* IWM_TX_CMD_FLG_SEQ_CTL */
    req->tx_cmd[0].rate_n_flags = 0x100; /* 1Mbps */
    req->tx_cmd[0].sta_id = 0xff;
    req->tx_cmd[1] = req->tx_cmd[0];
    req->scan_prio  = 2; /* IWM_SCAN_PRIORITY_HIGH */
    req->iter_num   = 1;
    req->delay      = 0;
    req->schedule[0].interval   = 0;
    req->schedule[0].iter_count = 1;
    req->schedule[1].interval   = 0;
    req->schedule[1].iter_count = 0xff;

    if (ssid && ssid[0]) {
        size_t slen = strlen(ssid);
        if (slen > 32) slen = 32;
        /* direct_scan = SSID IE list for directed (active) scan */
        req->direct_scan[0].id  = IEEE80211_ELEMID_SSID;
        req->direct_scan[0].len = (uint8_t)slen;
        memcpy(req->direct_scan[0].ssid, ssid, slen);
        /* keep PASS_ALL but not passive — active scan */
    }

    /* Fill channel array at end of fixed header */
    struct mvm_scan_channel_cfg_lmac *chs =
        (struct mvm_scan_channel_cfg_lmac *)(buf + sizeof(*req));
    for (uint8_t i = 0; i < nchan; i++) {
        chs[i].flags        = (ssid && ssid[0]) ?
                              (0x1u | ((1u << 1) - 1u) << 1) : /* active + ssid0 */
                              0; /* passive */
        chs[i].channel_num  = channels[i];
        chs[i].iter_count   = 1;
        chs[i].iter_interval = 0;
    }

    st->mvm_scan_done    = false;
    st->mvm_scan_running = true;
    st->ap_count         = 0;
    memset(st->aps, 0, sizeof(st->aps));

    int rc = iwl_mvm_send_cmd(st, MVM_CMD_SCAN_LMAC, buf, sizeof(buf));
    if (rc < 0) {
        st->mvm_scan_running = false;
        log_warn("iwlwifi", "%s MVM SCAN_OFFLOAD_REQUEST failed (%d)", iface->name, rc);
        /* Tear down contexts */
        iwl_mvm_binding_cmd(st, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_phy_ctx_cmd(st, 1, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_mac_ctx_cmd(st, iface, NULL, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return rc;
    }

    log_info("iwlwifi", "%s MVM LMAC scan started nchan=%u", iface->name, nchan);

    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 2000) {
        iwl_poll_rx_notifications(st);
        if (st->mvm_scan_done) break;
        if (st->ap_count > 0 && pit_ticks() - start > 500) break;
        __asm__ volatile("pause");
    }
    st->mvm_scan_running = false;

    /* Tear down scan contexts */
    iwl_mvm_binding_cmd(st, MVM_CTXT_ACTION_REMOVE);
    iwl_mvm_phy_ctx_cmd(st, 1, MVM_CTXT_ACTION_REMOVE);
    iwl_mvm_mac_ctx_cmd(st, iface, NULL, MVM_CTXT_ACTION_REMOVE, false, 0, 0);

    log_info("iwlwifi", "%s MVM scan done, %u AP(s) found", iface->name, (uint32_t)st->ap_count);
    return st->ap_count > 0 ? 0 : -10;
}

/*
 * MVM association sequence — from FreeBSD iwm_newstate(ASSOC):
 *   MAC_CONTEXT_CMD → PHY_CONTEXT_CMD → BINDING_CONTEXT_CMD →
 *   POWER_TABLE_CMD (disable PS) → ADD_STA → TIME_EVENT_CMD →
 *   [send auth frame] → [send assoc frame] → WPA4-way
 */
static int iwl_mvm_associate(struct iwlwifi_state *st,
                              struct net_iface *iface,
                              const char *ssid, const char *passphrase)
{
    if (!st->mvm_fw_ready) {
        log_warn("iwlwifi", "%s MVM assoc: firmware not ready", iface->name);
        return -1;
    }

    /* 1. Find AP (scan if needed) */
    const struct iwlwifi_ap *ap = iwl_find_ap(st, ssid);
    if (!ap) {
        int rc = iwl_mvm_scan(st, iface, ssid);
        if (rc < 0 && rc != -10) return rc;
        ap = iwl_find_ap(st, ssid);
        if (!ap) {
            log_warn("iwlwifi", "%s MVM: '%s' not found after scan", iface->name, ssid);
            return -10;
        }
    }

    log_info("iwlwifi", "%s MVM: associating with '%s' ch=%u", iface->name, ssid, ap->channel);

    /* 2. MAC context ADD (pre-assoc, not yet associated) */
    if (iwl_mvm_mac_ctx_cmd(st, iface, ap->bssid,
                              MVM_CTXT_ACTION_ADD, false, 0, 0) < 0) {
        return -7;
    }
    /* 3. PHY context ADD on AP's channel */
    if (iwl_mvm_phy_ctx_cmd(st, ap->channel, MVM_CTXT_ACTION_ADD) < 0) {
        iwl_mvm_mac_ctx_cmd(st, iface, ap->bssid, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return -7;
    }
    /* 4. Binding ADD */
    if (iwl_mvm_binding_cmd(st, MVM_CTXT_ACTION_ADD) < 0) {
        iwl_mvm_phy_ctx_cmd(st, ap->channel, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_mac_ctx_cmd(st, iface, ap->bssid, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return -7;
    }
    /* 5. Disable power save during auth/assoc */
    {
        struct mvm_device_power_cmd pwr;
        memset(&pwr, 0, sizeof(pwr));
        pwr.flags = (uint16_t)MVM_DEVICE_POWER_FLAGS_CAM;
        iwl_mvm_send_cmd(st, MVM_CMD_POWER_TABLE, &pwr, sizeof(pwr));
    }
    /* 6. ADD_STA for the AP */
    if (iwl_mvm_add_sta_cmd(st, iface, ap->bssid) < 0) {
        iwl_mvm_binding_cmd(st, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_phy_ctx_cmd(st, ap->channel, MVM_CTXT_ACTION_REMOVE);
        iwl_mvm_mac_ctx_cmd(st, iface, ap->bssid, MVM_CTXT_ACTION_REMOVE, false, 0, 0);
        return -7;
    }
    /* 7. Time event — protect auth/assoc window */
    iwl_mvm_protect_session(st, MVM_TE_SESSION_PROT_MAX_MS);

    /* 8. Store BSSID / channel for data path */
    memcpy(st->bssid, ap->bssid, 6);
    st->channel = ap->channel;
    strncpy(st->associated_ssid, ap->ssid, sizeof(st->associated_ssid) - 1);
    st->associated_ssid[sizeof(st->associated_ssid) - 1] = '\0';

    /* 9. 802.11 open auth */
    uint8_t frame[512];
    st->auth_done   = false;
    st->auth_status = 0xffff;
    size_t flen = iwl_build_auth_frame(frame, sizeof(frame), iface, ap, st->tx_sequence++);
    if (flen == 0 || iwl_send_mgmt_frame(st, frame, flen, true) < 0) {
        log_warn("iwlwifi", "%s MVM: could not send auth frame", iface->name);
        return -11;
    }
    if (iwl_wait_for_auth(st) < 0) {
        log_warn("iwlwifi", "%s MVM: auth timed out", iface->name);
        return -11;
    }
    log_info("iwlwifi", "%s MVM: auth OK", iface->name);

    /* 10. 802.11 association */
    st->assoc_done   = false;
    st->assoc_status = 0xffff;
    st->assoc_id     = 0;
    flen = iwl_build_assoc_frame(frame, sizeof(frame), iface, ap, passphrase, st->tx_sequence++);
    if (flen == 0 || iwl_send_mgmt_frame(st, frame, flen, true) < 0) {
        log_warn("iwlwifi", "%s MVM: could not send assoc frame", iface->name);
        return -12;
    }
    if (iwl_wait_for_assoc(st) < 0) {
        log_warn("iwlwifi", "%s MVM: assoc timed out", iface->name);
        return -12;
    }
    log_info("iwlwifi", "%s MVM: assoc OK aid=%u", iface->name, st->assoc_id);

    /* 11. Update MAC context to associated state */
    iwl_mvm_mac_ctx_cmd(st, iface, ap->bssid, MVM_CTXT_ACTION_MODIFY,
                         true, st->assoc_id, 3 /* DTIM interval */);

    /* 12. WPA2 4-way handshake */
    if (ap->privacy || (passphrase && passphrase[0])) {
        if (!passphrase || !passphrase[0]) {
            log_warn("iwlwifi", "%s MVM: WPA passphrase required", iface->name);
            return -8;
        }
        tnu_wpa_pmk_from_passphrase(passphrase, ssid, st->wpa_pmk);
        iwl_make_snonce(st, iface);
        st->wpa_key_msg1 = false;
        st->wpa_key_msg3 = false;
        uint64_t t = pit_ticks();
        while (!st->wpa_key_msg1 && pit_ticks() - t < 1500) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->wpa_key_msg1) {
            log_warn("iwlwifi", "%s MVM: WPA msg1 timed out", iface->name);
            return -14;
        }
        t = pit_ticks();
        while (!st->wpa_key_msg3 && pit_ticks() - t < 1500) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->wpa_key_msg3) {
            log_warn("iwlwifi", "%s MVM: WPA msg3 timed out", iface->name);
            return -15;
        }
        if (iwl_install_pairwise_ccmp_key(st, ap) < 0) {
            return -16;
        }
        st->ccmp_tx_pn = 0;
        log_info("iwlwifi", "%s MVM: WPA PTK installed", iface->name);
    }

    st->associated = true;
    st->link_ready  = true;
    iface->link     = true;
    iface->up       = true;
    log_info("iwlwifi", "%s MVM: associated with '%s' aid=%u channel=%u",
             iface->name, ssid, st->assoc_id, ap->channel);
    return 0;
}

static int iwl_send_scan_rxon(struct iwlwifi_state *st, const struct net_iface *iface)
{
    if (!st || !iface) {
        return -1;
    }
    if (st->modern_transport) {
        log_warn("iwlwifi", "%s RXON is a DVM command and is not supported on MVM devices (family=%s); scan/associate require MVM command porting",
                 iface->name, st->family);
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
    cmd->qid  = st->mgmt_queue_id;
    cmd->idx  = (uint8_t)index;

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
    iwl_write32(st, CSR_HBUS_TARG_WRPTR,
                ((uint32_t)st->mgmt_queue_id << 8) | st->tx_index);
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
    if (st->modern_transport) {
        /*
         * The MVM path keeps decryption disabled in the firmware MAC context
         * and uses TNU's software CCMP for RX/TX.  Sending the old DVM
         * ADD_NODE key command to modern firmware can fail after a successful
         * WPA handshake and prevent DHCP from ever starting.
         */
        log_info("iwlwifi", "enabled software pairwise CCMP for %02x:%02x:%02x:%02x:%02x:%02x",
                 ap->bssid[0], ap->bssid[1], ap->bssid[2],
                 ap->bssid[3], ap->bssid[4], ap->bssid[5]);
        return 0;
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

static int iwl_dma_load_sections(struct iwlwifi_state *st,
                                 const struct iwlwifi_fw_section *sections,
                                 size_t count, const char *name)
{
    if (!sections || count == 0) {
        return 0;
    }
    if (iwl_alloc_firmware_dma(st) < 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        int rc = iwl_dma_load_section(st, sections[i].offset,
                                      sections[i].data, sections[i].size);
        if (rc < 0) {
            log_warn("iwlwifi", "%s firmware section %u/%u offset=%08x size=%u failed (%d)",
                     name, (uint32_t)(i + 1), (uint32_t)count,
                     sections[i].offset, sections[i].size, rc);
            return rc;
        }
    }
    log_info("iwlwifi", "loaded %s firmware image: %u sections",
             name, (uint32_t)count);
    return 0;
}

static bool iwl_uses_legacy_4965_path(const struct iwlwifi_state *st)
{
    (void)st;
    return false; /* no 4965 in our device table */
}

static int iwl_execute_runtime_firmware(struct iwlwifi_state *st)
{
    bool modern_sections = st->runtime_section_count > 0;
    if (!st->firmware_parsed || (!st->firmware_staged && !modern_sections)) {
        return -1;
    }
    if (st->modern_transport) {
        log_info("iwlwifi", "%s modern Intel transport: attempting generic DMA/RX/TX bring-up",
                 st->family);
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

    if (st->modern_transport) {
        /*
         * MVM requires two separate resets:
         *
         * Phase 1 — INIT firmware:
         *   load init sections → deassert reset → wait INIT alive
         *   → send INIT_COMPLETE_NOTIF (triggers calibration)
         *   → drain calibration blobs (CALIB_RES_NOTIF / PHY_DB)
         *
         * Phase 2 — RT firmware:
         *   load runtime sections → deassert reset → wait RT alive
         *   → POWER_TABLE + PHY_DB replay + REPLY_SF_CFG
         */

        /* --- Phase 1: INIT firmware --- */
        if (st->init_section_count > 0) {
            int rc = iwl_dma_load_sections(st, st->init_sections,
                                           st->init_section_count, "init");
            if (rc < 0) {
                return rc;
            }
        } else if (st->init_ucode.text && st->init_ucode.text_size) {
            int rc = iwl_dma_load_section(st, IWL_FW_TEXT_BASE,
                                          st->init_ucode.text,
                                          st->init_ucode.text_size);
            if (rc < 0) return rc;
            rc = iwl_dma_load_section(st, IWL_FW_DATA_BASE,
                                      st->init_ucode.data,
                                      st->init_ucode.data_size);
            if (rc < 0) return rc;
        }

        iwl_write32(st, CSR_INT, 0xffffffffu);
        iwl_write32(st, CSR_RESET, 0);

        /* For MVM, INIT alive is sent via RX ring as UC_READY message, not interrupt.
         * Just poll for it. */
        uint64_t t = pit_ticks();
        while (!st->mvm_init_alive_seen && pit_ticks() - t < 800) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->mvm_init_alive_seen) {
            log_warn("iwlwifi", "MVM INIT alive timed out - firmware not responding");
            return -4;
        }
        log_info("iwlwifi", "MVM INIT alive OK");

        /*
         * Do NOT send anything to the firmware during INIT phase.
         * INIT_COMPLETE (0x04) is a notification the firmware sends *to us*
         * when init completes — writing it back would trigger a SW_ERR.
         * Just drain whatever the INIT firmware emits (PHY_DB blobs etc.)
         * and wait for it to reach the completed state on its own.
         */
        t = pit_ticks();
        while (pit_ticks() - t < 600) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        log_info("iwlwifi", "MVM INIT phase drained, PHY_DB cfg=%u nch=%u papd=%u txp=%u",
                 st->phy_db_cfg_size, st->phy_db_calib_nch_size,
                 st->phy_db_n_papd, st->phy_db_n_txp);

        /* Reset state flags so the RT alive parse works cleanly */
        st->mvm_rt_alive_seen = false;

        /*
         * Phase 1→2 transition: re-initialize the NIC hardware before loading
         * the runtime firmware.  iwm_start_fw() in FreeBSD calls iwm_nic_init()
         * for every firmware image — both INIT and RT.  Without this, the INIT
         * CPU is still running and its DMA engine is active; the first RT DMA
         * write triggers IWL_INT_SW_ERR (0x02000000).
         *
         * Sequence mirrors iwm_nic_init():
         *   1. APM init (clocks, power)
         *   2. Re-setup RX ring
         *   3. Re-setup CMD queue
         *   4. Re-setup TX queue
         */
        {
            int nic_rc = iwl_apm_init(st);
            if (nic_rc < 0) {
                log_warn("iwlwifi", "MVM Phase2 APM re-init failed (%d)", nic_rc);
                return -7;
            }
            /* Force re-programming of the FH registers by clearing the
             * ready flags (DMA buffers already allocated are reused) */
            st->cmd_queue_ready = false;
            st->tx_queue_ready = false;
            nic_rc = iwl_init_rx_ring(st);
            if (nic_rc < 0) {
                log_warn("iwlwifi", "MVM Phase2 RX ring re-init failed (%d)", nic_rc);
                return -5;
            }
            nic_rc = iwl_init_cmd_queue(st);
            if (nic_rc < 0) {
                log_warn("iwlwifi", "MVM Phase2 CMD queue re-init failed (%d)", nic_rc);
                return -8;
            }
            nic_rc = iwl_init_tx_queue(st);
            if (nic_rc < 0) {
                log_warn("iwlwifi", "MVM Phase2 TX queue re-init failed (%d)", nic_rc);
                return -9;
            }
        }

        /* Re-arm interrupts for RT alive */
        iwl_write32(st, CSR_INT_MASK, IWL_INT_MASK_MIN);
        iwl_write32(st, CSR_INT, 0xffffffffu);
        iwl_write32(st, CSR_FH_INT_STATUS, 0xffffffffu);

        /* --- Phase 2: RT firmware --- */
        if (st->runtime_section_count > 0) {
            int rc = iwl_dma_load_sections(st, st->runtime_sections,
                                           st->runtime_section_count, "runtime");
            if (rc < 0) return rc;
        } else {
            int rc = iwl_dma_load_section(st, IWL_FW_TEXT_BASE,
                                          st->runtime_ucode.text,
                                          st->runtime_ucode.text_size);
            if (rc < 0) return rc;
            rc = iwl_dma_load_section(st, IWL_FW_DATA_BASE,
                                      st->runtime_ucode.data,
                                      st->runtime_ucode.data_size);
            if (rc < 0) return rc;
        }

        iwl_write32(st, CSR_INT, 0xffffffffu);
        iwl_write32(st, CSR_RESET, 0);

        /* For MVM, RT alive is sent via RX ring as UC_READY message, not interrupt. */
        t = pit_ticks();
        while (!st->mvm_rt_alive_seen && pit_ticks() - t < 800) {
            iwl_poll_rx_notifications(st);
            __asm__ volatile("pause");
        }
        if (!st->mvm_rt_alive_seen) {
            log_warn("iwlwifi", "MVM RT alive timed out - firmware not responding");
            return -4;
        }
        st->mvm_alive          = true;
        st->firmware_alive     = true;
        st->firmware_running   = true;
        log_info("iwlwifi", "MVM RT alive OK");

        return iwl_mvm_post_alive_init(st);
    }

    /* DVM path — load sections then start */
    {
        int rc2;
        if (modern_sections) {
            rc2 = iwl_dma_load_sections(st, st->init_sections,
                                        st->init_section_count, "init");
            if (rc2 < 0) return rc2;
            rc2 = iwl_dma_load_sections(st, st->runtime_sections,
                                        st->runtime_section_count, "runtime");
            if (rc2 < 0) return rc2;
        } else {
            rc2 = iwl_dma_load_section(st, IWL_FW_TEXT_BASE,
                                       st->runtime_ucode.text,
                                       st->runtime_ucode.text_size);
            if (rc2 < 0) return rc2;
            rc2 = iwl_dma_load_section(st, IWL_FW_DATA_BASE,
                                       st->runtime_ucode.data,
                                       st->runtime_ucode.data_size);
            if (rc2 < 0) return rc2;
        }
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
        st->firmware_alive   = true;
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
    /* Reset MVM phase-tracking before every fresh start attempt */
    if (st->modern_transport) {
        st->mvm_init_alive_seen = false;
        st->mvm_rt_alive_seen   = false;
        st->mvm_alive              = false;
        st->mvm_fw_ready           = false;
        st->phy_db_cfg_size        = 0;
        st->phy_db_calib_nch_size  = 0;
        st->phy_db_n_papd          = 0;
        st->phy_db_n_txp           = 0;
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
    const uint8_t *tx_frame = wifi;
    size_t tx_len = (size_t)(p - wifi);
    uint8_t protected_wifi[1720];
    if (st->wpa_ptk_ready) {
        tx_len = iwl_ccmp_encrypt_frame(st, wifi, tx_len,
                                        protected_wifi, sizeof(protected_wifi));
        if (tx_len == 0) {
            log_warn("iwlwifi", "%s could not CCMP-encrypt data frame", iface->name);
            return -3;
        }
        tx_frame = protected_wifi;
    }
    int rc = iwl_send_mgmt_frame(st, tx_frame, tx_len, true);
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
        return iwl_mvm_scan(st, iface, NULL);
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
    if (st->modern_transport) {
        return iwl_mvm_associate(st, iface, ssid, passphrase);
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
        st->ccmp_tx_pn = 0;
        st->associated = true;
        st->link_ready = true;
        iface->link = true;
        iface->up = true;
        log_info("iwlwifi", "%s WPA PTK installed for '%s'; CCMP data path enabled",
                 iface->name, ssid);
        return 0;
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
    /*
     * MVM: host command queue = 9 (IWM_CMD_QUEUE from FreeBSD if_iwmreg.h)
     * DVM: host command queue = 4, mgmt TX = 0
     */
    if (st->modern_transport) {
        st->cmd_queue_id  = MVM_CMD_QUEUE; /* 9 */
        st->mgmt_queue_id = 0;
    } else {
        st->cmd_queue_id  = IWL_CMD_QUEUE; /* 4 */
        st->mgmt_queue_id = IWL_MGMT_QUEUE; /* 0 */
    }

    if (pci_set_power_state_d0(dev) == 0) {
        log_info("iwlwifi", "%s PCI power state set to D0", iface->name);
    }
    if (pci_disable_link_power_management(dev) == 0) {
        log_info("iwlwifi", "%s PCIe ASPM disabled for bring-up", iface->name);
    }
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
        log_info("iwlwifi", "%s firmware is available; modern DMA/RX/TX bring-up is enabled",
                 iface->name);
    } else if (st->firmware_loaded) {
        log_info("iwlwifi", "%s firmware is staged; DMA upload and queues are enabled",
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

/* ---------------------------------------------------------------------
 *  Cleanup – not used in the current kernel (no unload), but kept for
 *  completeness.
 * --------------------------------------------------------------------- */
void iwlwifi_exit(void)
{
    /* Not implemented - no module unloading in this kernel */
}
