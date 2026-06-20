#ifndef TNU_NET_H
#define TNU_NET_H

#include <tnu/ioctl.h>
#include <tnu/types.h>

#define NET_IFACE_MAX 8
#define NET_NAME_MAX 15

#define WIFI_IOCTL_SCAN    _IOR('w', 0x01, struct wifi_ap[32])
#define WIFI_IOCTL_CONNECT _IOW('w', 0x02, struct wifi_connect_req)
#define WIFI_IOCTL_STATUS  _IOR('w', 0x03, struct wifi_status)

struct wifi_ap {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint16_t flags;
};

#define WIFI_AP_PRIVACY  (1u << 0)
#define WIFI_AP_HIDDEN   (1u << 1)
#define WIFI_AP_WEP      (1u << 2)
#define WIFI_AP_WPA      (1u << 3)
#define WIFI_AP_WPA2     (1u << 4)
#define WIFI_AP_WPA3     (1u << 5)

struct wifi_connect_req {
    char ssid[32];
    char psk[64];
};

struct wifi_status {
    bool connected;
    char ssid[32];
};

struct net_iface;

typedef void (*net_rx_callback_t)(struct net_iface *iface, const uint8_t *frame,
                                  size_t len, void *ctx);

struct net_driver_ops {
    int (*transmit)(struct net_iface *iface, const void *frame, size_t len);
    void (*poll)(struct net_iface *iface, net_rx_callback_t callback, void *ctx);
};

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
    const struct net_driver_ops *ops;
    uint32_t arp_ip;
    uint8_t arp_mac[6];
    bool arp_valid;
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
int net_iface_dhcp(const char *name);
int net_wifi_scan(void);
int net_wifi_connect(const char *iface, const char *ssid, const char *passphrase);
int net_wifi_autoconnect(void);
int net_wifi_scan_results(struct wifi_ap *out, size_t max_aps);
int net_wifi_status(struct wifi_status *out);

/* Socket API (stub implementations for Linux compatibility) */
int net_socket_create(int domain, int type, int protocol);
bool net_socket_is_open(int sockfd);
int net_socket_connect(int sockfd, uint32_t ip, uint16_t port);
ssize_t net_socket_send(int sockfd, const void *buf, size_t len);
ssize_t net_socket_recv(int sockfd, void *buf, size_t len);
int net_socket_close(int sockfd);

#endif
