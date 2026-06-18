# Intel Wi-Fi Support in TNU

TNU includes a comprehensive native iwlwifi driver that supports Intel wireless adapters without requiring Linux compatibility layers. The driver is implemented entirely in the TNU kernel and communicates directly with Intel Wi-Fi hardware.

## Supported Hardware

The driver supports Intel wireless adapters across multiple generations:

### MVM (Modern) Firmware Devices
- Intel Wireless-AC 7260, 7265, 8260, 8265
- Intel Wireless-AC 9000 series (9260, 9460, 9560)
- Intel Wireless-AX 200, 201, 210 series
- Intel Wi-Fi 6/6E (AX200, AX201, AX210, AX211)
- Intel Wi-Fi 7 (AX411, AX210, AX211)

### DVM (Legacy) Firmware Devices
- Intel Wireless-N 100, 1000, 105, 135
- Intel Wireless-N 2000, 2030, 2200
- Intel Centrino Ultimate-N 6300, Advanced-N 6200, 6205, 6230
- Intel Wireless-N 1030, 130
- Intel PRO/Wireless 3945ABG, 4965AGN
- Intel Wireless WiFi Link 5100, 5300, 5150, 5350
- Intel WiFi Link 1000 BGN
- Intel Centrino Advanced-N 6200, 6205, 6230

## Firmware

Firmware files must be present in `/lib/firmware/iwlwifi/`. The TNU build system includes firmware files in the ISO image at:

```
/lib/firmware/iwlwifi/
├── iwlwifi-7260-17.ucode
├── iwlwifi-7265-17.ucode
├── iwlwifi-3160-17.ucode
├── iwlwifi-3168-29.ucode
├── iwlwifi-8265-36.ucode
├── iwlwifi-9000-pu-b0-jf-b0-46.ucode
├── iwlwifi-9260-th-b0-jf-b0-46.ucode
├── iwlwifi-Qu-b0-hr-b0-77.ucode
├── iwlwifi-cc-a0-77.ucode
├── iwlwifi-so-a0-hr-b0-89.ucode
├── iwlwifi-ty-a0-gf-a0-89.ucode
├── iwlwifi-bz-b0-fm-c0-97.ucode
├── iwlwifi-ma-b0-hr-b0-89.ucode
└── ... (many more)
```

The driver automatically selects the correct firmware based on the PCI device ID.

## Usage

### Check Network Interfaces

```bash
# List all network interfaces
ifconfig

# Check if Wi-Fi device was detected
dmesg | grep iwlwifi
```

### Scan for Networks

```bash
# Scan for available Wi-Fi networks
wifi scan
```

Output example:
```
MyNetwork  12:34:56:78:9a:bc  rssi=-42  wpa
GuestWiFi  aa:bb:cc:dd:ee:ff  rssi=-65  open
```

### Connect to a Network

```bash
# Connect to an open network
wifi connect wlan0 MyOpenNetwork

# Connect to a WPA2-PSK protected network
wifi connect wlan0 MyNetwork mypassphrase

# Or use the full form
wifi connect wlan0 MyNetwork "my secure passphrase"
```

### Check Connection Status

```bash
wifi status
```

Output example:
```
connected: MyNetwork
```

### Configure IP Address

After connecting to Wi-Fi, you typically need to obtain an IP address via DHCP:

```bash
# Request DHCP lease (if not automatic)
dhclient wlan0

# Or manually configure
ifconfig wlan0 192.168.1.100 netmask 255.255.255.0
route add default gw 192.168.1.1
```

## Auto-Connect on Boot

TNU supports auto-connecting to configured networks on boot. Create a profile file:

```
/etc/wifi/profile
```

With the following format:

```
iface=wlan0
ssid=MyNetwork
passphrase=mypassphrase
autoconnect=true
```

The `bootd` daemon will attempt to connect automatically during boot.

## Driver Architecture

The iwlwifi driver is implemented in `kernel/drivers/net/iwlwifi.c` and consists of:

1. **PCI Device Detection** - Scans PCI bus for Intel wireless devices
2. **BAR Mapping** - Maps MMIO registers for hardware communication
3. **Firmware Loading** - Loads TLV-format firmware from `/lib/firmware/`
4. **DMA Management** - Allocates and manages DMA buffers for TX/RX
5. **MVM/DVM Support** - Handles both modern (MVM) and legacy (DVM) firmware
6. **MAC/PHY Context** - Manages hardware contexts for operation
7. **Scanning** - Implements active and passive channel scanning
8. **Association** - Handles 802.11 authentication and association
9. **WPA2-PSK** - Implements 4-way handshake and CCMP encryption
10. **Data Path** - RX/TX handling for data frames

### Key Functions

| Function | Description |
|----------|-------------|
| `iwlwifi_attach()` | Initialize device, map BAR, load firmware |
| `iwlwifi_start()` | Start firmware execution |
| `iwlwifi_scan()` | Scan for available networks |
| `iwlwifi_associate()` | Connect to a specific network |
| `iwlwifi_poll()` | Poll for received frames |

## Kernel Configuration

The driver is enabled by default. Key configuration options:

- `IWL_MAX_DEVICES` - Maximum number of supported Wi-Fi adapters (default: 8)
- `IWL_MMIO_SIZE` - MMIO region size (default: 128KB)
- `IWL_FW_DMA_SIZE` - Firmware DMA buffer size (default: 512KB)

## Troubleshooting

### No Wi-Fi Device Found

1. Check if the device is detected by PCI:
   ```bash
   lspci
   ```
   
2. Check kernel logs:
   ```bash
   dmesg | grep -i wifi
   dmesg | grep -i iwl
   ```

3. Verify the device is Intel:
   - Vendor ID should be `0x8086`
   - Class code should be `0x028000` (Network controller, Wireless)

### Firmware Load Failure

1. Check if firmware files exist:
   ```bash
   ls /lib/firmware/iwlwifi/
   ```

2. Check kernel logs for specific errors:
   ```bash
   dmesg | grep "firmware"
   ```

3. Ensure the correct firmware version is present for your device

### Association Fails

1. Verify the SSID and passphrase are correct
2. Check if the AP is in range:
   ```bash
   wifi scan
   ```
   
3. Check kernel logs:
   ```bash
   dmesg | grep -i "assoc\|auth"
   ```

### No IP Address After Connect

1. Check if DHCP is working:
   ```bash
   dhclient wlan0
   ```

2. Manually configure if DHCP fails:
   ```bash
   ifconfig wlan0 192.168.1.100 netmask 255.255.255.0
   ```

## Network Stack Integration

The iwlwifi driver integrates with TNU's network stack (`kernel/drivers/net/net.c`):

- **Interface Management** - Creates `wlan0`, `wlan1`, etc. interfaces
- **ARP** - Handles ARP requests for IP resolution
- **IPv4** - Processes IPv4 packets
- **ICMP** - Responds to ping requests
- **UDP** - Supports DNS queries
- **DHCP** - Can request IP addresses via DHCP client

## Security

The driver supports:

- **WPA2-PSK** - Personal (pre-shared key) authentication
- **CCMP** - AES encryption for data frames
- **Open Networks** - No encryption (use with caution)

WPA3-SAE is not yet implemented.

## Limitations

Current limitations:

1. No 5 GHz band preference configuration
2. No power save mode (always in CAM mode)
3. No WPA3-SAE support
4. No AP mode (station mode only)
5. No multi-BSS support

## Development

To contribute to the Wi-Fi stack:

1. **Driver Code**: `kernel/drivers/net/iwlwifi.c`
2. **Header**: `kernel/include/tnu/iwlwifi.h`
3. **Network Stack**: `kernel/drivers/net/net.c`
4. **Userspace API**: `userspace/libc/src/syscall.c`
5. **Utility**: `userspace/coreutils/tnu-utils.c` (wifi command)

### Debugging

Enable verbose logging by modifying the driver:

```c
// In kernel/drivers/net/iwlwifi.c
#define IWL_DEBUG_VERBOSE 1
```

This will output detailed firmware and hardware interaction logs.
