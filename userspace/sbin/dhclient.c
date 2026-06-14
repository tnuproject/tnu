/*
 * dhclient - DHCP Client for TNU
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#ifndef htonl
#define htonl(x) ((uint32_t)((x) << 24) | (((x) << 8) & 0xFF0000) | (((x) >> 8) & 0xFF00) | ((x) >> 24))
#endif
#ifndef ntohl
#define ntohl(x) htonl(x)
#endif

/* DHCP packet structure */
#define DHCP_MAGIC_COOKIE 0x63825363

typedef struct {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;
    uint8_t options[308];
} dhcp_packet_t;

/* DHCP message types */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

/* DHCP options */
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS_SERVER    6
#define OPT_DOMAIN_NAME   15
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_PARAM_REQ     55
#define OPT_CLIENT_ID     61
#define OPT_END           255

/* Network interface info */
static uint8_t mac_addr[6];
static char interface[32];

int dhcp_send_packet(int sock, uint8_t msg_type, uint32_t xid, 
                     uint32_t requested_ip, uint32_t server_id)
{
    dhcp_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    
    packet.op = 1; /* BOOTREQUEST */
    packet.htype = 1; /* Ethernet */
    packet.hlen = 6;
    packet.xid = xid;
    packet.secs = 0;
    packet.flags = 0x8000; /* Broadcast */
    
    memcpy(packet.chaddr, mac_addr, 6);
    packet.cookie = DHCP_MAGIC_COOKIE;
    
    /* Build options */
    uint8_t *opt = packet.options;
    
    /* Message type */
    *opt++ = OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = msg_type;
    
    /* Client ID */
    *opt++ = OPT_CLIENT_ID;
    *opt++ = 7;
    *opt++ = 1; /* Ethernet */
    memcpy(opt, mac_addr, 6);
    opt += 6;
    
    if (msg_type == DHCP_REQUEST) {
        /* Requested IP */
        if (requested_ip) {
            *opt++ = OPT_REQUESTED_IP;
            *opt++ = 4;
            memcpy(opt, &requested_ip, 4);
            opt += 4;
        }
        
        /* Server ID */
        if (server_id) {
            *opt++ = OPT_SERVER_ID;
            *opt++ = 4;
            memcpy(opt, &server_id, 4);
            opt += 4;
        }
    }
    
    /* Parameter request list */
    *opt++ = OPT_PARAM_REQ;
    *opt++ = 4;
    *opt++ = OPT_SUBNET_MASK;
    *opt++ = OPT_ROUTER;
    *opt++ = OPT_DNS_SERVER;
    *opt++ = OPT_DOMAIN_NAME;
    
    /* End */
    *opt++ = OPT_END;
    
    /* Send packet (broadcast to 255.255.255.255:67) */
    /* TODO: Implement UDP socket send */
    
    return 0;
}

int dhcp_parse_response(const uint8_t *data, size_t len,
                        uint32_t *ip, uint32_t *netmask, uint32_t *gateway,
                        uint32_t *dns, uint32_t *server, uint32_t *lease_time)
{
    if (len < sizeof(dhcp_packet_t)) return -1;
    
    const dhcp_packet_t *packet = (const dhcp_packet_t *)data;
    
    *ip = packet->yiaddr;
    
    /* Parse options */
    const uint8_t *opt = packet->options;
    const uint8_t *end = (const uint8_t *)packet + len;
    
    while (opt < end && *opt != OPT_END) {
        uint8_t type = *opt++;
        if (opt >= end) break;
        uint8_t len = *opt++;
        
        switch (type) {
        case OPT_SUBNET_MASK:
            if (len == 4) memcpy(netmask, opt, 4);
            break;
        case OPT_ROUTER:
            if (len >= 4) memcpy(gateway, opt, 4);
            break;
        case OPT_DNS_SERVER:
            if (len >= 4) memcpy(dns, opt, 4);
            break;
        case OPT_SERVER_ID:
            if (len == 4) memcpy(server, opt, 4);
            break;
        case OPT_LEASE_TIME:
            if (len == 4) {
                uint32_t lt;
                memcpy(&lt, opt, 4);
                *lease_time = ntohl(lt);
            }
            break;
        }
        
        opt += len;
    }
    
    return 0;
}

int dhclient_main(const char *iface)
{
    strncpy(interface, iface, sizeof(interface) - 1);
    
    uint32_t xid = (uint32_t)time(NULL);
    
    printf("dhclient: sending DHCPDISCOVER on %s\n", iface);
    
    /* Send DHCPDISCOVER */
    dhcp_send_packet(0, DHCP_DISCOVER, xid, 0, 0);
    
    /* Wait for DHCPOFFER */
    /* TODO: Implement socket receive with timeout */
    
    /* Send DHCPREQUEST */
    printf("dhclient: sending DHCPREQUEST\n");
    
    /* Wait for DHCPACK */
    
    /* Configure interface */
    printf("dhclient: configured %s\n", iface);
    
    return 0;
}

int main(int argc, char **argv)
{
    const char *iface = "eth0";
    
    if (argc > 1) {
        iface = argv[1];
    }
    
    return dhclient_main(iface);
}
