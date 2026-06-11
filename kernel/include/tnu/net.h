#ifndef TNU_NET_H
#define TNU_NET_H

#include <tnu/types.h>

#define NET_IFACE_MAX 8
#define NET_NAME_MAX 15

enum net_iface_type {
    NET_IFACE_LOOPBACK = 1,
    NET_IFACE_ETHERNET = 2,
    NET_IFACE_WIFI = 3,
};

struct net_iface {
    char name[NET_NAME_MAX + 1];
    enum net_iface_type type;
    bool up;
    bool link;
    uint8_t mac[6];
    uint32_t ipv4;
    uint32_t netmask;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint32_t gateway;
    uint32_t dns_server;
    const char *driver;
    const char *ssid;
    bool configurable;
    void *driver_data;
};

void net_init(void);
size_t net_iface_count(void);
const struct net_iface *net_iface_get(size_t index);
const struct net_iface *net_iface_find(const char *name);
const char *net_iface_type_name(enum net_iface_type type);
int net_ping4(uint32_t ipv4, uint16_t sequence, uint32_t *latency_ms);
int net_resolve4(const char *host, uint32_t *out_ipv4);
uint32_t net_parse_ipv4(const char *s);
void net_format_ipv4(uint32_t ip, char *out, size_t out_size);
bool net_has_external_transport(void);
int net_iface_configure_ipv4(const char *name, uint32_t ipv4, uint32_t netmask, uint32_t gateway);
int net_iface_set_up(const char *name, bool up);
int net_wifi_scan(void);
int net_wifi_connect(const char *iface, const char *ssid, const char *passphrase);

#endif
