#include <arch/pci.h>
#include <arch/pit.h>
#include <tnu/log.h>
#include <tnu/memory.h>
#include <tnu/iwlwifi.h>
#include <tnu/net.h>
#include <tnu/printf.h>
#include <tnu/string.h>
#include <tnu/vfs.h>

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define DNS_PORT 53
#define DNS_SOURCE_PORT 49153
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63825363u

#define E1000_MAX 4
#define E1000_RX_COUNT 32
#define E1000_TX_COUNT 8
#define E1000_RX_BUF_SIZE 2048
#define E1000_TX_BUF_SIZE 2048
#define E1000_MMIO_SIZE (128 * 1024)

#define E1000_REG_CTRL   0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_IMC    0x00d8
#define E1000_REG_RCTL   0x0100
#define E1000_REG_TCTL   0x0400
#define E1000_REG_TIPG   0x0410
#define E1000_REG_RDBAL  0x2800
#define E1000_REG_RDBAH  0x2804
#define E1000_REG_RDLEN  0x2808
#define E1000_REG_RDH    0x2810
#define E1000_REG_RDT    0x2818
#define E1000_REG_TDBAL  0x3800
#define E1000_REG_TDBAH  0x3804
#define E1000_REG_TDLEN  0x3808
#define E1000_REG_TDH    0x3810
#define E1000_REG_TDT    0x3818
#define E1000_REG_RAL0   0x5400
#define E1000_REG_RAH0   0x5404

#define E1000_CTRL_SLU   (1u << 6)
#define E1000_STATUS_LU  (1u << 1)
#define E1000_RCTL_EN    (1u << 1)
#define E1000_RCTL_BAM   (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN    (1u << 1)
#define E1000_TCTL_PSP   (1u << 3)
#define E1000_TX_CMD_EOP 0x01
#define E1000_TX_CMD_IFCS 0x02
#define E1000_TX_CMD_RS  0x08
#define E1000_TX_STATUS_DD 0x01
#define E1000_RX_STATUS_DD 0x01

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

struct e1000_state {
    uintptr_t mmio;
    struct e1000_rx_desc rx[E1000_RX_COUNT] __attribute__((aligned(16)));
    struct e1000_tx_desc tx[E1000_TX_COUNT] __attribute__((aligned(16)));
    uint8_t rx_buf[E1000_RX_COUNT][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
    uint8_t tx_buf[E1000_TX_COUNT][E1000_TX_BUF_SIZE] __attribute__((aligned(16)));
    uint16_t rx_index;
    uint16_t tx_index;
};

struct ping_wait {
    uint32_t target;
    uint16_t id;
    uint16_t sequence;
    bool complete;
};

struct dns_wait {
    uint16_t id;
    uint16_t port;
    uint32_t server;
    uint32_t answer;
    bool complete;
};

struct dhcp_wait {
    uint32_t xid;
    uint8_t expected_type;
    uint32_t offered_ip;
    uint32_t server_id;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    bool complete;
};

struct poll_ctx {
    struct ping_wait *ping;
    struct dns_wait *dns;
    struct dhcp_wait *dhcp;
};

static struct net_iface ifaces[NET_IFACE_MAX];
static size_t iface_count;
static struct e1000_state e1000_states[E1000_MAX];
static size_t e1000_count;

static struct net_iface *net_iface_find_mut(const char *name);

static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t ipv4_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static uint16_t inet_checksum(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static bool name_ends_with(const char *name, const char *suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           strcmp(name + name_len - suffix_len, suffix) == 0;
}

static struct net_iface *add_iface(const char *name, enum net_iface_type type)
{
    if (iface_count >= NET_IFACE_MAX) {
        return NULL;
    }
    struct net_iface *iface = &ifaces[iface_count++];
    memset(iface, 0, sizeof(*iface));
    strncpy(iface->name, name, NET_NAME_MAX);
    iface->type = type;
    iface->driver = "none";
    return iface;
}

static bool is_wifi(const struct pci_device *dev)
{
    return dev->class_code == 0x02 && dev->subclass == 0x80;
}

static bool is_intel_wifi(const struct pci_device *dev)
{
    return is_wifi(dev) && dev->vendor_id == 0x8086;
}

static bool is_realtek_wifi(const struct pci_device *dev)
{
    return is_wifi(dev) && dev->vendor_id == 0x10ec;
}

static bool is_ethernet(const struct pci_device *dev)
{
    return dev->class_code == 0x02 && dev->subclass == 0x00;
}

static bool is_supported_e1000(const struct pci_device *dev)
{
    if (dev->vendor_id != 0x8086) {
        return false;
    }
    switch (dev->device_id) {
    case 0x1000: case 0x1001: case 0x1004: case 0x1008:
    case 0x1009: case 0x100c: case 0x100d: case 0x100e:
    case 0x100f: case 0x1010: case 0x1011: case 0x1012:
    case 0x1013: case 0x1015: case 0x1016: case 0x1017:
    case 0x1018: case 0x1019: case 0x101d: case 0x1026:
    case 0x1027: case 0x1028: case 0x1075: case 0x1076:
    case 0x1077:
        return true;
    default:
        return false;
    }
}

static void synthetic_mac(struct net_iface *iface, const struct pci_device *dev)
{
    iface->mac[0] = 0x02;
    iface->mac[1] = 0x54;
    iface->mac[2] = dev->vendor_id & 0xff;
    iface->mac[3] = dev->vendor_id >> 8;
    iface->mac[4] = dev->device_id & 0xff;
    iface->mac[5] = dev->slot;
}

static uint32_t e1000_read(const struct e1000_state *st, uint32_t reg)
{
    return *(volatile uint32_t *)(st->mmio + reg);
}

static void e1000_write(const struct e1000_state *st, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(st->mmio + reg) = value;
}

static bool mac_is_usable(const uint8_t mac[6])
{
    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < 6; i++) {
        if (mac[i] != 0x00) {
            all_zero = false;
        }
        if (mac[i] != 0xff) {
            all_ff = false;
        }
    }
    return !all_zero && !all_ff;
}

static void e1000_read_mac(struct e1000_state *st, struct net_iface *iface,
                           const struct pci_device *dev)
{
    uint32_t ral = e1000_read(st, E1000_REG_RAL0);
    uint32_t rah = e1000_read(st, E1000_REG_RAH0);
    iface->mac[0] = (uint8_t)(ral & 0xff);
    iface->mac[1] = (uint8_t)((ral >> 8) & 0xff);
    iface->mac[2] = (uint8_t)((ral >> 16) & 0xff);
    iface->mac[3] = (uint8_t)((ral >> 24) & 0xff);
    iface->mac[4] = (uint8_t)(rah & 0xff);
    iface->mac[5] = (uint8_t)((rah >> 8) & 0xff);
    if (!mac_is_usable(iface->mac)) {
        synthetic_mac(iface, dev);
        ral = (uint32_t)iface->mac[0] | ((uint32_t)iface->mac[1] << 8) |
              ((uint32_t)iface->mac[2] << 16) | ((uint32_t)iface->mac[3] << 24);
        rah = (uint32_t)iface->mac[4] | ((uint32_t)iface->mac[5] << 8) | (1u << 31);
        e1000_write(st, E1000_REG_RAL0, ral);
        e1000_write(st, E1000_REG_RAH0, rah);
    }
}

static int e1000_transmit(struct net_iface *iface, const void *data, size_t len)
{
    struct e1000_state *st = iface->driver_data;
    if (!st || !data || len > E1000_TX_BUF_SIZE) {
        return -1;
    }
    size_t copy_len = len;
    size_t tx_len = len < 60 ? 60 : len;

    uint16_t index = st->tx_index;
    struct e1000_tx_desc *desc = &st->tx[index];
    for (size_t wait = 0; wait < 1000000 && !(desc->status & E1000_TX_STATUS_DD); wait++) {
        __asm__ volatile("pause");
    }
    if (!(desc->status & E1000_TX_STATUS_DD)) {
        return -1;
    }

    memset(st->tx_buf[index], 0, tx_len);
    memcpy(st->tx_buf[index], data, copy_len);
    desc->addr = (uint64_t)(uintptr_t)st->tx_buf[index];
    desc->length = (uint16_t)tx_len;
    desc->cso = 0;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    st->tx_index = (uint16_t)((index + 1) % E1000_TX_COUNT);
    e1000_write(st, E1000_REG_TDT, st->tx_index);

    for (size_t wait = 0; wait < 1000000 && !(desc->status & E1000_TX_STATUS_DD); wait++) {
        __asm__ volatile("pause");
    }
    iface->tx_packets++;
    iface->tx_bytes += tx_len;
    return (desc->status & E1000_TX_STATUS_DD) ? 0 : -1;
}

static void e1000_poll(struct net_iface *iface, net_rx_callback_t callback, void *ctx)
{
    struct e1000_state *st = iface->driver_data;
    if (!st || !callback) {
        return;
    }
    for (size_t count = 0; count < E1000_RX_COUNT; count++) {
        struct e1000_rx_desc *desc = &st->rx[st->rx_index];
        if (!(desc->status & E1000_RX_STATUS_DD)) {
            break;
        }
        size_t len = desc->length;
        if (len <= E1000_RX_BUF_SIZE) {
            iface->rx_packets++;
            iface->rx_bytes += len;
            callback(iface, st->rx_buf[st->rx_index], len, ctx);
        }
        desc->status = 0;
        e1000_write(st, E1000_REG_RDT, st->rx_index);
        st->rx_index = (uint16_t)((st->rx_index + 1) % E1000_RX_COUNT);
    }
}

static const struct net_driver_ops e1000_ops = {
    .transmit = e1000_transmit,
    .poll = e1000_poll,
};

static void build_eth(uint8_t *frame, const uint8_t dst[6],
                      const uint8_t src[6], uint16_t type)
{
    memcpy(frame, dst, 6);
    memcpy(frame + 6, src, 6);
    put16(frame + 12, type);
}

static int send_arp(struct net_iface *iface, uint32_t target_ip,
                    const uint8_t target_mac[6], uint16_t op)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t frame[60];
    memset(frame, 0, sizeof(frame));
    build_eth(frame, op == 1 ? broadcast : target_mac, iface->mac, ETH_TYPE_ARP);
    put16(frame + 14, 1);
    put16(frame + 16, ETH_TYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    put16(frame + 20, op);
    memcpy(frame + 22, iface->mac, 6);
    put32(frame + 28, iface->ipv4);
    if (op == 2) {
        memcpy(frame + 32, target_mac, 6);
    }
    put32(frame + 38, target_ip);
    if (!iface->ops || !iface->ops->transmit) {
        return -1;
    }
    return iface->ops->transmit(iface, frame, sizeof(frame));
}

static void send_arp_reply(struct net_iface *iface, const uint8_t *arp)
{
    uint8_t requester_mac[6];
    memcpy(requester_mac, arp + 8, 6);
    uint32_t requester_ip = be32(arp + 14);
    send_arp(iface, requester_ip, requester_mac, 2);
}

static int send_icmp_echo(struct net_iface *iface, const uint8_t dst_mac[6],
                          uint32_t target_ip, uint16_t id, uint16_t sequence)
{
    uint8_t frame[14 + 20 + 8 + 32];
    memset(frame, 0, sizeof(frame));
    build_eth(frame, dst_mac, iface->mac, ETH_TYPE_IPV4);

    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, 20 + 8 + 32);
    put16(ip + 4, sequence);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_ICMP;
    put32(ip + 12, iface->ipv4);
    put32(ip + 16, target_ip);
    put16(ip + 10, inet_checksum(ip, 20));

    uint8_t *icmp = ip + 20;
    icmp[0] = 8;
    icmp[1] = 0;
    put16(icmp + 4, id);
    put16(icmp + 6, sequence);
    for (size_t i = 0; i < 32; i++) {
        icmp[8 + i] = (uint8_t)('A' + (i % 26));
    }
    put16(icmp + 2, inet_checksum(icmp, 8 + 32));
    if (!iface->ops || !iface->ops->transmit) {
        return -1;
    }
    return iface->ops->transmit(iface, frame, sizeof(frame));
}

static int send_udp4_from(struct net_iface *iface, const uint8_t dst_mac[6],
                          uint32_t source_ip, uint32_t target_ip,
                          uint16_t src_port, uint16_t dst_port,
                          const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len > 512) {
        return -1;
    }
    uint8_t frame[14 + 20 + 8 + 512];
    size_t ip_len = 20 + 8 + payload_len;
    size_t frame_len = 14 + ip_len;
    memset(frame, 0, sizeof(frame));
    build_eth(frame, dst_mac, iface->mac, ETH_TYPE_IPV4);

    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)ip_len);
    put16(ip + 4, src_port);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    put32(ip + 12, source_ip);
    put32(ip + 16, target_ip);
    put16(ip + 10, inet_checksum(ip, 20));

    uint8_t *udp = ip + 20;
    put16(udp, src_port);
    put16(udp + 2, dst_port);
    put16(udp + 4, (uint16_t)(8 + payload_len));
    put16(udp + 6, 0);
    memcpy(udp + 8, payload, payload_len);
    if (!iface->ops || !iface->ops->transmit) {
        return -1;
    }
    return iface->ops->transmit(iface, frame, frame_len);
}

static int send_udp4(struct net_iface *iface, const uint8_t dst_mac[6],
                     uint32_t target_ip, uint16_t src_port, uint16_t dst_port,
                     const uint8_t *payload, size_t payload_len)
{
    return send_udp4_from(iface, dst_mac, iface->ipv4, target_ip,
                          src_port, dst_port, payload, payload_len);
}

static void process_arp(struct net_iface *iface, const uint8_t *payload, size_t len)
{
    if (len < 28 || be16(payload) != 1 || be16(payload + 2) != ETH_TYPE_IPV4 ||
        payload[4] != 6 || payload[5] != 4) {
        return;
    }
    uint16_t op = be16(payload + 6);
    uint32_t sender_ip = be32(payload + 14);
    uint32_t target_ip = be32(payload + 24);

    if (op == 2 && target_ip == iface->ipv4) {
        iface->arp_ip = sender_ip;
        memcpy(iface->arp_mac, payload + 8, 6);
        iface->arp_valid = true;
    } else if (op == 1 && target_ip == iface->ipv4) {
        send_arp_reply(iface, payload);
    }
}

static int dns_skip_name(const uint8_t *msg, size_t msg_len, size_t *offset)
{
    size_t pos = *offset;
    size_t guard = 0;
    while (pos < msg_len && guard++ < msg_len) {
        uint8_t len = msg[pos++];
        if (len == 0) {
            *offset = pos;
            return 0;
        }
        if ((len & 0xc0) == 0xc0) {
            if (pos >= msg_len) {
                return -1;
            }
            *offset = pos + 1;
            return 0;
        }
        if (len & 0xc0) {
            return -1;
        }
        if (pos + len > msg_len) {
            return -1;
        }
        pos += len;
    }
    return -1;
}

static void process_dns(struct dns_wait *dns, const uint8_t *payload, size_t len,
                        uint32_t src_ip, uint16_t src_port, uint16_t dst_port)
{
    if (!dns || dns->complete || len < 12 || src_ip != dns->server ||
        src_port != DNS_PORT || dst_port != dns->port || be16(payload) != dns->id) {
        return;
    }

    uint16_t flags = be16(payload + 2);
    if (!(flags & 0x8000) || (flags & 0x000f) != 0) {
        return;
    }

    uint16_t qdcount = be16(payload + 4);
    uint16_t ancount = be16(payload + 6);
    size_t offset = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        if (dns_skip_name(payload, len, &offset) < 0 || offset + 4 > len) {
            return;
        }
        offset += 4;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        if (dns_skip_name(payload, len, &offset) < 0 || offset + 10 > len) {
            return;
        }
        uint16_t type = be16(payload + offset);
        uint16_t class_code = be16(payload + offset + 2);
        uint16_t rdlen = be16(payload + offset + 8);
        offset += 10;
        if (offset + rdlen > len) {
            return;
        }
        if (type == 1 && class_code == 1 && rdlen == 4) {
            dns->answer = be32(payload + offset);
            dns->complete = true;
            return;
        }
        offset += rdlen;
    }
}

static void process_dhcp(struct dhcp_wait *dhcp, const uint8_t *payload, size_t len,
                         uint16_t src_port, uint16_t dst_port)
{
    if (!dhcp || dhcp->complete || src_port != DHCP_SERVER_PORT ||
        dst_port != DHCP_CLIENT_PORT || len < 240) {
        return;
    }
    if (payload[0] != 2 || payload[1] != 1 || payload[2] != 6 ||
        be32(payload + 4) != dhcp->xid || be32(payload + 236) != DHCP_MAGIC) {
        return;
    }

    uint8_t msg_type = 0;
    uint32_t server_id = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns_server = 0;
    size_t off = 240;
    while (off < len) {
        uint8_t opt = payload[off++];
        if (opt == 0) {
            continue;
        }
        if (opt == 255) {
            break;
        }
        if (off >= len) {
            return;
        }
        uint8_t opt_len = payload[off++];
        if (off + opt_len > len) {
            return;
        }
        switch (opt) {
        case 1:
            if (opt_len >= 4) {
                netmask = be32(payload + off);
            }
            break;
        case 3:
            if (opt_len >= 4) {
                gateway = be32(payload + off);
            }
            break;
        case 6:
            if (opt_len >= 4) {
                dns_server = be32(payload + off);
            }
            break;
        case 53:
            if (opt_len >= 1) {
                msg_type = payload[off];
            }
            break;
        case 54:
            if (opt_len >= 4) {
                server_id = be32(payload + off);
            }
            break;
        default:
            break;
        }
        off += opt_len;
    }

    if (msg_type != dhcp->expected_type) {
        return;
    }
    dhcp->offered_ip = be32(payload + 16);
    dhcp->server_id = server_id;
    dhcp->netmask = netmask ? netmask : ipv4_make(255, 255, 255, 0);
    dhcp->gateway = gateway;
    dhcp->dns_server = dns_server ? dns_server : gateway;
    dhcp->complete = dhcp->offered_ip != 0;
}

static void process_ipv4(struct net_iface *iface, const uint8_t *payload, size_t len,
                         struct poll_ctx *ctx)
{
    if (len < 20 || (payload[0] >> 4) != 4) {
        return;
    }
    size_t ihl = (payload[0] & 0x0f) * 4;
    uint32_t dst_ip = be32(payload + 16);
    if (ihl < 20 || len < ihl + 8 ||
        (dst_ip != iface->ipv4 && dst_ip != ipv4_make(255, 255, 255, 255))) {
        return;
    }
    uint32_t src_ip = be32(payload + 12);
    if (payload[9] == IP_PROTO_ICMP) {
        const uint8_t *icmp = payload + ihl;
        if (ctx && ctx->ping && icmp[0] == 0 && be16(icmp + 4) == ctx->ping->id &&
            be16(icmp + 6) == ctx->ping->sequence && src_ip == ctx->ping->target) {
            ctx->ping->complete = true;
        }
    } else if (payload[9] == IP_PROTO_UDP) {
        const uint8_t *udp = payload + ihl;
        uint16_t src_port = be16(udp);
        uint16_t dst_port = be16(udp + 2);
        uint16_t udp_len = be16(udp + 4);
        if (udp_len >= 8 && ihl + udp_len <= len) {
            process_dhcp(ctx ? ctx->dhcp : NULL, udp + 8, udp_len - 8,
                         src_port, dst_port);
            process_dns(ctx ? ctx->dns : NULL, udp + 8, udp_len - 8,
                        src_ip, src_port, dst_port);
        }
    }
}

static void process_frame_ctx(struct net_iface *iface, const uint8_t *frame,
                              size_t len, void *opaque)
{
    struct poll_ctx *ctx = opaque;
    if (len < 14) {
        return;
    }
    uint16_t type = be16(frame + 12);
    if (type == ETH_TYPE_ARP) {
        process_arp(iface, frame + 14, len - 14);
    } else if (type == ETH_TYPE_IPV4) {
        process_ipv4(iface, frame + 14, len - 14, ctx);
    }
}

static void net_poll_iface(struct net_iface *iface, struct ping_wait *ping,
                           struct dns_wait *dns, struct dhcp_wait *dhcp)
{
    if (!iface->ops || !iface->ops->poll) {
        return;
    }
    struct poll_ctx ctx = {
        .ping = ping,
        .dns = dns,
        .dhcp = dhcp,
    };
    iface->ops->poll(iface, process_frame_ctx, &ctx);
}

static bool same_subnet(uint32_t a, uint32_t b, uint32_t mask)
{
    return (a & mask) == (b & mask);
}

static int resolve_mac(struct net_iface *iface, uint32_t ip, uint8_t out[6])
{
    if (!iface->ops || !iface->ops->transmit || !iface->ops->poll) {
        return -1;
    }
    if (iface->arp_valid && iface->arp_ip == ip) {
        memcpy(out, iface->arp_mac, 6);
        return 0;
    }
    if (send_arp(iface, ip, NULL, 1) < 0) {
        return -1;
    }
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 200) {
        net_poll_iface(iface, NULL, NULL, NULL);
        if (iface->arp_valid && iface->arp_ip == ip) {
            memcpy(out, iface->arp_mac, 6);
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static void dhcp_put_option(uint8_t *buf, size_t *pos, uint8_t opt,
                            const void *data, uint8_t len)
{
    buf[(*pos)++] = opt;
    buf[(*pos)++] = len;
    memcpy(buf + *pos, data, len);
    *pos += len;
}

static int send_dhcp_message(struct net_iface *iface, uint8_t msg_type,
                             uint32_t xid, uint32_t requested_ip,
                             uint32_t server_id)
{
    static const uint8_t broadcast_mac[6] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t packet[300];
    memset(packet, 0, sizeof(packet));
    packet[0] = 1;
    packet[1] = 1;
    packet[2] = 6;
    put32(packet + 4, xid);
    put16(packet + 10, 0x8000);
    memcpy(packet + 28, iface->mac, 6);
    put32(packet + 236, DHCP_MAGIC);

    size_t pos = 240;
    dhcp_put_option(packet, &pos, 53, &msg_type, 1);

    uint8_t client_id[7];
    client_id[0] = 1;
    memcpy(client_id + 1, iface->mac, 6);
    dhcp_put_option(packet, &pos, 61, client_id, sizeof(client_id));

    uint8_t params[] = { 1, 3, 6 };
    dhcp_put_option(packet, &pos, 55, params, sizeof(params));

    if (requested_ip) {
        uint8_t ip_opt[4];
        put32(ip_opt, requested_ip);
        dhcp_put_option(packet, &pos, 50, ip_opt, sizeof(ip_opt));
    }
    if (server_id) {
        uint8_t server_opt[4];
        put32(server_opt, server_id);
        dhcp_put_option(packet, &pos, 54, server_opt, sizeof(server_opt));
    }

    if (pos < sizeof(packet)) {
        packet[pos++] = 255;
    }
    while (pos < sizeof(packet)) {
        packet[pos++] = 0;
    }

    return send_udp4_from(iface, broadcast_mac, 0,
                          ipv4_make(255, 255, 255, 255),
                          DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                          packet, sizeof(packet));
}

int net_iface_dhcp(const char *name)
{
    struct net_iface *iface = net_iface_find_mut(name);
    if (!iface || !iface->up || !iface->link || !iface->ops ||
        !iface->ops->transmit || !iface->ops->poll || !mac_is_usable(iface->mac)) {
        return -1;
    }

    uint32_t xid = 0x544e0000u | ((uint32_t)pit_ticks() & 0xffffu);
    struct dhcp_wait offer = {
        .xid = xid,
        .expected_type = 2,
    };
    iface->ipv4 = 0;
    iface->gateway = 0;
    iface->dns_server = 0;
    iface->arp_valid = false;

    if (send_dhcp_message(iface, 1, xid, 0, 0) < 0) {
        return -2;
    }
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < 500) {
        net_poll_iface(iface, NULL, NULL, &offer);
        if (offer.complete) {
            break;
        }
        __asm__ volatile("pause");
    }
    if (!offer.complete) {
        return -3;
    }

    struct dhcp_wait ack = {
        .xid = xid,
        .expected_type = 5,
    };
    if (send_dhcp_message(iface, 3, xid, offer.offered_ip, offer.server_id) < 0) {
        return -4;
    }
    start = pit_ticks();
    while (pit_ticks() - start < 500) {
        net_poll_iface(iface, NULL, NULL, &ack);
        if (ack.complete) {
            break;
        }
        __asm__ volatile("pause");
    }
    if (!ack.complete) {
        return -5;
    }

    iface->ipv4 = ack.offered_ip;
    iface->netmask = ack.netmask;
    iface->gateway = ack.gateway;
    iface->dns_server = ack.dns_server;
    iface->arp_valid = false;
    log_info("dhcp", "%s lease ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u",
             iface->name,
             (iface->ipv4 >> 24) & 0xff, (iface->ipv4 >> 16) & 0xff,
             (iface->ipv4 >> 8) & 0xff, iface->ipv4 & 0xff,
             (iface->gateway >> 24) & 0xff, (iface->gateway >> 16) & 0xff,
             (iface->gateway >> 8) & 0xff, iface->gateway & 0xff,
             (iface->dns_server >> 24) & 0xff, (iface->dns_server >> 16) & 0xff,
             (iface->dns_server >> 8) & 0xff, iface->dns_server & 0xff);
    return 0;
}

static int e1000_init_iface(struct net_iface *iface, const struct pci_device *dev)
{
    if (e1000_count >= E1000_MAX) {
        return -1;
    }
    uint32_t bar0 = dev->bars[0];
    if ((bar0 & 1u) || bar0 == 0 || bar0 == 0xffffffffu) {
        return -1;
    }
    uintptr_t mmio = (uintptr_t)(bar0 & ~0x0fu);
    if (vmm_map_range_identity(mmio, E1000_MMIO_SIZE, 0) < 0) {
        log_warn("e1000", "%s BAR0 %p could not be mapped", iface->name, (void *)mmio);
        return -1;
    }

    struct e1000_state *st = &e1000_states[e1000_count++];
    memset(st, 0, sizeof(*st));
    st->mmio = mmio;
    iface->driver_data = st;
    iface->driver = "e1000";
    iface->configurable = true;
    iface->ops = &e1000_ops;

    pci_enable_bus_mastering(dev);
    e1000_write(st, E1000_REG_IMC, 0xffffffffu);
    e1000_write(st, E1000_REG_RCTL, 0);
    e1000_write(st, E1000_REG_TCTL, 0);
    e1000_write(st, E1000_REG_CTRL, e1000_read(st, E1000_REG_CTRL) | E1000_CTRL_SLU);
    e1000_read_mac(st, iface, dev);

    for (size_t i = 0; i < E1000_RX_COUNT; i++) {
        st->rx[i].addr = (uint64_t)(uintptr_t)st->rx_buf[i];
        st->rx[i].status = 0;
    }
    for (size_t i = 0; i < E1000_TX_COUNT; i++) {
        st->tx[i].addr = (uint64_t)(uintptr_t)st->tx_buf[i];
        st->tx[i].status = E1000_TX_STATUS_DD;
    }

    e1000_write(st, E1000_REG_RDBAL, (uint32_t)(uintptr_t)st->rx);
    e1000_write(st, E1000_REG_RDBAH, (uint32_t)(((uint64_t)(uintptr_t)st->rx) >> 32));
    e1000_write(st, E1000_REG_RDLEN, E1000_RX_COUNT * sizeof(struct e1000_rx_desc));
    e1000_write(st, E1000_REG_RDH, 0);
    e1000_write(st, E1000_REG_RDT, E1000_RX_COUNT - 1);
    st->rx_index = 0;

    e1000_write(st, E1000_REG_TDBAL, (uint32_t)(uintptr_t)st->tx);
    e1000_write(st, E1000_REG_TDBAH, (uint32_t)(((uint64_t)(uintptr_t)st->tx) >> 32));
    e1000_write(st, E1000_REG_TDLEN, E1000_TX_COUNT * sizeof(struct e1000_tx_desc));
    e1000_write(st, E1000_REG_TDH, 0);
    e1000_write(st, E1000_REG_TDT, 0);
    st->tx_index = 0;

    e1000_write(st, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write(st, E1000_REG_TIPG, 10 | (8 << 10) | (6 << 20));
    e1000_write(st, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                   (0x10 << 4) | (0x40 << 12));

    iface->up = true;
    iface->link = (e1000_read(st, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
    if (!iface->link) {
        iface->link = true;
    }
    iface->ipv4 = ipv4_make(10, 0, 2, 15);
    iface->netmask = ipv4_make(255, 255, 255, 0);
    iface->gateway = ipv4_make(10, 0, 2, 2);
    iface->dns_server = ipv4_make(10, 0, 2, 3);

    log_info("e1000", "%s mmio=%p mac=%02x:%02x:%02x:%02x:%02x:%02x ip=10.0.2.15",
             iface->name, (void *)mmio, iface->mac[0], iface->mac[1], iface->mac[2],
             iface->mac[3], iface->mac[4], iface->mac[5]);
    return 0;
}

void net_init(void)
{
    iface_count = 0;
    e1000_count = 0;

    struct net_iface *lo = add_iface("lo", NET_IFACE_LOOPBACK);
    if (lo) {
        lo->up = true;
        lo->link = true;
        lo->configurable = true;
        lo->driver = "loopback";
        lo->ipv4 = ipv4_make(127, 0, 0, 1);
        lo->netmask = ipv4_make(255, 0, 0, 0);
    }

    size_t eth_index = 0;
    size_t wifi_index = 0;
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *dev = pci_get(i);
        enum net_iface_type type;
        char name[NET_NAME_MAX + 1];
        if (is_ethernet(dev)) {
            type = NET_IFACE_ETHERNET;
            ksnprintf(name, sizeof(name), "eth%u", (uint32_t)eth_index++);
        } else if (is_wifi(dev)) {
            type = NET_IFACE_WIFI;
            ksnprintf(name, sizeof(name), "wlan%u", (uint32_t)wifi_index++);
        } else {
            continue;
        }

        struct net_iface *iface = add_iface(name, type);
        if (!iface) {
            continue;
        }
        iface->pci_vendor = dev->vendor_id;
        iface->pci_device = dev->device_id;
        synthetic_mac(iface, dev);

        if (type == NET_IFACE_ETHERNET && is_supported_e1000(dev) &&
            e1000_init_iface(iface, dev) == 0) {
            continue;
        }
        if (type == NET_IFACE_WIFI && iwlwifi_is_supported(dev) &&
            iwlwifi_attach(iface, dev) == 0) {
            continue;
        }

        iface->up = false;
        iface->link = false;
        if (is_intel_wifi(dev)) {
            iface->driver = "iwlwifi-pending";
        } else if (is_realtek_wifi(dev)) {
            iface->driver = "rtw-pending";
        } else {
            iface->driver = "unsupported";
        }
        log_info("net", "%s %02x:%02x.%u vendor=%04x device=%04x driver=%s",
                 iface->name, dev->bus, dev->slot, dev->function,
                 dev->vendor_id, dev->device_id, iface->driver);
        if (type == NET_IFACE_WIFI) {
            log_warn("wifi", "%s detected, but firmware loading, 802.11 MAC, and WPA association are not complete",
                     iface->name);
        }
    }
    if (iface_count == 1) {
        log_info("net", "no Ethernet or Wi-Fi PCI devices detected");
    }
    log_info("net", "network core ready");
}

size_t net_iface_count(void)
{
    return iface_count;
}

const struct net_iface *net_iface_get(size_t index)
{
    return index < iface_count ? &ifaces[index] : NULL;
}

const struct net_iface *net_iface_find(const char *name)
{
    for (size_t i = 0; i < iface_count; i++) {
        if (strcmp(ifaces[i].name, name) == 0) {
            return &ifaces[i];
        }
    }
    return NULL;
}

static struct net_iface *net_iface_find_mut(const char *name)
{
    return (struct net_iface *)net_iface_find(name);
}

const char *net_iface_type_name(enum net_iface_type type)
{
    switch (type) {
    case NET_IFACE_LOOPBACK:
        return "loopback";
    case NET_IFACE_ETHERNET:
        return "ethernet";
    case NET_IFACE_WIFI:
        return "wifi";
    default:
        return "unknown";
    }
}

int net_iface_configure_ipv4(const char *name, uint32_t ipv4, uint32_t netmask, uint32_t gateway)
{
    struct net_iface *iface = net_iface_find_mut(name);
    if (!iface || !iface->configurable || !ipv4 || !netmask) {
        return -1;
    }
    iface->ipv4 = ipv4;
    iface->netmask = netmask;
    iface->gateway = gateway;
    if (!iface->dns_server && gateway) {
        iface->dns_server = gateway;
    }
    iface->up = true;
    return 0;
}

int net_iface_set_up(const char *name, bool up)
{
    struct net_iface *iface = net_iface_find_mut(name);
    if (!iface || !iface->configurable) {
        return -1;
    }
    iface->up = up;
    return 0;
}

static struct net_iface *route_for(uint32_t ip)
{
    for (size_t i = 0; i < iface_count; i++) {
        struct net_iface *iface = &ifaces[i];
        if (iface->up && iface->link && iface->ipv4 &&
            iface->ops && iface->ops->transmit && iface->ops->poll) {
            if (same_subnet(ip, iface->ipv4, iface->netmask) || iface->gateway) {
                return iface;
            }
        }
    }
    return NULL;
}

int net_ping4(uint32_t ipv4, uint16_t sequence, uint32_t *latency_ms)
{
    if (ipv4 == ipv4_make(127, 0, 0, 1)) {
        if (latency_ms) {
            *latency_ms = 0;
        }
        ifaces[0].tx_packets++;
        ifaces[0].rx_packets++;
        ifaces[0].tx_bytes += 64;
        ifaces[0].rx_bytes += 64;
        return 0;
    }

    struct net_iface *iface = route_for(ipv4);
    if (!iface) {
        return -1;
    }

    uint32_t next_hop = same_subnet(ipv4, iface->ipv4, iface->netmask) ? ipv4 : iface->gateway;
    uint8_t dst_mac[6];
    if (!next_hop || resolve_mac(iface, next_hop, dst_mac) < 0) {
        return -1;
    }

    struct ping_wait ping = {
        .target = ipv4,
        .id = 0x544e,
        .sequence = sequence,
        .complete = false,
    };
    uint64_t start = pit_ticks();
    if (send_icmp_echo(iface, dst_mac, ipv4, ping.id, sequence) < 0) {
        return -1;
    }
    while (pit_ticks() - start < 300) {
        net_poll_iface(iface, &ping, NULL, NULL);
        if (ping.complete) {
            if (latency_ms) {
                *latency_ms = (uint32_t)((pit_ticks() - start) * 10);
            }
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int dns_build_query(const char *host, uint16_t id, uint8_t *out, size_t *out_len)
{
    if (!host || !host[0] || !out || !out_len) {
        return -1;
    }
    memset(out, 0, 512);
    put16(out, id);
    put16(out + 2, 0x0100);
    put16(out + 4, 1);
    size_t pos = 12;
    const char *label = host;
    while (*label) {
        const char *dot = label;
        while (*dot && *dot != '.') {
            dot++;
        }
        size_t len = (size_t)(dot - label);
        if (len == 0 || len > 63 || pos + 1 + len + 5 > 512) {
            return -1;
        }
        out[pos++] = (uint8_t)len;
        for (size_t i = 0; i < len; i++) {
            char c = label[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-';
            if (!ok) {
                return -1;
            }
            out[pos++] = (uint8_t)c;
        }
        label = *dot == '.' ? dot + 1 : dot;
    }
    out[pos++] = 0;
    put16(out + pos, 1);
    pos += 2;
    put16(out + pos, 1);
    pos += 2;
    *out_len = pos;
    return 0;
}

int net_resolve4(const char *host, uint32_t *out_ipv4)
{
    if (!host || !out_ipv4) {
        return -1;
    }
    uint32_t numeric = net_parse_ipv4(host);
    if (numeric) {
        *out_ipv4 = numeric;
        return 0;
    }

    struct net_iface *iface = route_for(ipv4_make(1, 1, 1, 1));
    if (!iface || !iface->dns_server) {
        return -1;
    }

    uint8_t query[512];
    size_t query_len = 0;
    uint16_t id = 0x544e;
    if (dns_build_query(host, id, query, &query_len) < 0) {
        return -1;
    }

    uint32_t server = iface->dns_server;
    uint32_t next_hop = same_subnet(server, iface->ipv4, iface->netmask) ? server : iface->gateway;
    uint8_t dst_mac[6];
    if (!next_hop || resolve_mac(iface, next_hop, dst_mac) < 0) {
        return -1;
    }

    struct dns_wait dns = {
        .id = id,
        .port = DNS_SOURCE_PORT,
        .server = server,
        .answer = 0,
        .complete = false,
    };
    uint64_t start = pit_ticks();
    if (send_udp4(iface, dst_mac, server, DNS_SOURCE_PORT, DNS_PORT, query, query_len) < 0) {
        return -1;
    }
    while (pit_ticks() - start < 300) {
        net_poll_iface(iface, NULL, &dns, NULL);
        if (dns.complete && dns.answer) {
            *out_ipv4 = dns.answer;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

uint32_t net_parse_ipv4(const char *s)
{
    uint32_t octets[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 4; i++) {
        if (!*s) {
            return 0;
        }
        uint32_t value = 0;
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (uint32_t)(*s - '0');
            if (value > 255) {
                return 0;
            }
            s++;
        }
        octets[i] = value;
        if (i < 3) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    return *s == '\0' ? ipv4_make(octets[0], octets[1], octets[2], octets[3]) : 0;
}

void net_format_ipv4(uint32_t ip, char *out, size_t out_size)
{
    ksnprintf(out, out_size, "%u.%u.%u.%u",
              (ip >> 24) & 0xff, (ip >> 16) & 0xff,
              (ip >> 8) & 0xff, ip & 0xff);
}

bool net_has_external_transport(void)
{
    return route_for(ipv4_make(1, 1, 1, 1)) != NULL;
}

int net_wifi_scan(void)
{
    bool saw_iwlwifi = false;
    bool iwl_transport_ready = false;
    int last_iwl_rc = -3;
    for (size_t i = 0; i < iface_count; i++) {
        if (ifaces[i].type == NET_IFACE_WIFI) {
            if (strcmp(ifaces[i].driver, "iwlwifi") == 0) {
                saw_iwlwifi = true;
                last_iwl_rc = iwlwifi_scan(&ifaces[i]);
                if (last_iwl_rc == 0) {
                    iwl_transport_ready = true;
                }
            }
        }
    }
    if (iwl_transport_ready) {
        return 0;
    }
    if (saw_iwlwifi) {
        return last_iwl_rc == -1 ? -3 : last_iwl_rc;
    }
    for (size_t i = 0; i < iface_count; i++) {
        if (ifaces[i].type == NET_IFACE_WIFI) {
            return -2;
        }
    }
    return -1;
}

int net_wifi_connect(const char *iface_name, const char *ssid, const char *passphrase)
{
    struct net_iface *iface = net_iface_find_mut(iface_name);
    if (!iface || iface->type != NET_IFACE_WIFI) {
        return -1;
    }
    if (strcmp(iface->driver, "iwlwifi") == 0) {
        const struct iwlwifi_state *st = iwlwifi_state_for(iface);
        int rc = iwlwifi_associate(iface, ssid, passphrase);
        if (rc == 0) {
            iface->up = true;
            iface->link = true;
            int dhcp = net_iface_dhcp(iface_name);
            if (dhcp < 0) {
                log_warn("iwlwifi", "%s associated with '%s' but DHCP failed (%d)",
                         iface_name, ssid, dhcp);
                return -4;
            }
            return 0;
        } else if (st && st->firmware_loaded) {
            log_warn("iwlwifi", "%s association blocked (%d)",
                     iface_name, rc);
        } else {
            log_warn("iwlwifi", "%s association blocked: firmware %s is not loaded",
                     iface_name, st && st->firmware_name ? st->firmware_name : "unknown");
        }
        return rc == -1 ? -3 : rc;
    }
    log_warn("wifi", "%s cannot associate with '%s': firmware/MAC/WPA layer unavailable",
             iface_name, ssid ? ssid : "");
    return -2;
}

struct wifi_profile {
    char iface[NET_NAME_MAX + 1];
    char ssid[33];
    char passphrase[65];
    bool autoconnect;
};

static void profile_value(const char *text, const char *key, char *out, size_t out_size)
{
    if (!text || !key || !out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t value_len = len - key_len - 1;
            if (value_len >= out_size) {
                value_len = out_size - 1;
            }
            memcpy(out, line + key_len + 1, value_len);
            out[value_len] = '\0';
            return;
        }
        if (!end) {
            return;
        }
        line = end + 1;
    }
}

static bool profile_bool_value(const char *text, const char *key)
{
    char value[8];
    profile_value(text, key, value, sizeof(value));
    return strcmp(value, "yes") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "1") == 0;
}

static int parse_wifi_profile(const struct vfs_node *node, struct wifi_profile *profile)
{
    if (!node || !node->data || node->size == 0 || !profile) {
        return -1;
    }
    char text[256];
    size_t len = node->size < sizeof(text) - 1 ? (size_t)node->size : sizeof(text) - 1;
    memcpy(text, node->data, len);
    text[len] = '\0';

    memset(profile, 0, sizeof(*profile));
    profile_value(text, "iface", profile->iface, sizeof(profile->iface));
    profile_value(text, "ssid", profile->ssid, sizeof(profile->ssid));
    profile_value(text, "passphrase", profile->passphrase, sizeof(profile->passphrase));
    profile->autoconnect = profile_bool_value(text, "autoconnect");
    if (!profile->iface[0] || !profile->ssid[0]) {
        return -1;
    }
    return 0;
}

struct wifi_autoconnect_ctx {
    int attempted;
    int connected;
};

static void wifi_autoconnect_emit(struct vfs_node *node, void *ctx)
{
    struct wifi_autoconnect_ctx *state = ctx;
    if (!node || node->type != VFS_NODE_FILE || !state ||
        !name_ends_with(node->name, ".conf")) {
        return;
    }

    struct wifi_profile profile;
    if (parse_wifi_profile(node, &profile) < 0 || !profile.autoconnect) {
        return;
    }
    state->attempted++;
    int rc = net_wifi_connect(profile.iface, profile.ssid, profile.passphrase);
    if (rc == 0) {
        state->connected++;
        log_info("wifi", "autoconnected %s to '%s'", profile.iface, profile.ssid);
    } else {
        log_warn("wifi", "autoconnect %s -> '%s' failed (%d)",
                 profile.iface, profile.ssid, rc);
    }
}

int net_wifi_autoconnect(void)
{
    struct vfs_node *dir = vfs_lookup("/etc/wifi", "/");
    if (!dir || dir->type != VFS_NODE_DIR) {
        return 0;
    }
    struct wifi_autoconnect_ctx ctx = {0};
    vfs_list(dir, wifi_autoconnect_emit, &ctx);
    return ctx.connected > 0 ? 0 : (ctx.attempted > 0 ? -1 : 0);
}

int net_wifi_scan_results(struct wifi_ap *out, size_t max_aps)
{
    if (!out || max_aps == 0) {
        return -1;
    }
    size_t count = 0;
    for (size_t i = 0; i < iface_count && count < max_aps; i++) {
        if (ifaces[i].type != NET_IFACE_WIFI || strcmp(ifaces[i].driver, "iwlwifi") != 0) {
            continue;
        }
        const struct iwlwifi_state *st = iwlwifi_state_for(&ifaces[i]);
        if (!st) {
            continue;
        }
        size_t n = st->ap_count < max_aps - count ? st->ap_count : max_aps - count;
        for (size_t j = 0; j < n; j++) {
            const struct iwlwifi_ap *ap = &st->aps[j];
            if (!ap->valid) {
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            strncpy(out[count].ssid, ap->ssid, sizeof(out[count].ssid) - 1);
            memcpy(out[count].bssid, ap->bssid, sizeof(out[count].bssid));
            out[count].channel = ap->channel;
            out[count].rssi = ap->rssi;
            out[count].flags = ap->security_flags;
            count++;
        }
    }
    return (int)count;
}

int net_wifi_status(struct wifi_status *out)
{
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < iface_count; i++) {
        if (ifaces[i].type != NET_IFACE_WIFI || strcmp(ifaces[i].driver, "iwlwifi") != 0) {
            continue;
        }
        const struct iwlwifi_state *st = iwlwifi_state_for(&ifaces[i]);
        if (!st) {
            continue;
        }
        if (st->associated && st->link_ready) {
            out->connected = true;
            strncpy(out->ssid, st->associated_ssid, sizeof(out->ssid) - 1);
            return 0;
        }
    }
    return 0;
}
