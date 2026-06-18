# Linux Driver Runtime (LDR)

LDR is the kernel-side compatibility runtime that lets selected Linux kernel
drivers act as backend providers for native Tiramisu subsystems.

The user experience remains native:

```text
wifi scan
wifi connect SSID PASSWORD
ping google.com
curl https://example.com
```

No Linux userspace tools are part of the control plane. Linux drivers are hidden
behind Tiramisu APIs.

## Source Tree

```text
kernel/
  compat/linux/driver/
    linux_driver_runtime.c      # module manager, PCI ownership, TNAI hooks
    linux_driver_kapi.c         # workqueue, IRQ, DMA, firmware, cfg80211/mac80211/i915 shims
  include/tnu/
    linux_driver_runtime.h      # public kernel API
```

Future split once the ABI grows:

```text
kernel/compat/linux/driver/
  module.c      # ELF .ko loader, relocation, symbol resolver
  kapi.c        # Linux exported symbol ABI
  workqueue.c   # work_struct, delayed_work, tasklets
  irq.c         # request_irq/free_irq bridge
  dma.c         # dma_map_*, coherent DMA, sync helpers
  pci.c         # pci_driver, pci_dev, config space bridge
  firmware.c    # request_firmware into /lib/firmware
  netdev.c      # Linux net_device <-> TNAI
  cfg80211.c    # scan/connect/disconnect/roam bridge
  mac80211.c    # ieee80211_hw bridge for softmac drivers
  i915.c        # DRM/KMS bridge to Tiramisu graphics
```

## Lifecycle

```text
INIT -> LOAD -> PROBE -> ATTACH -> RUNNING -> RECOVERY -> UNLOAD
```

Current implementation registers builtin module descriptors, validates staged
ELF64 relocatable module images, exposes device claims, and provides initial
workqueue, IRQ, DMA, firmware, cfg80211/mac80211 and i915 bridge shims.

Real `.ko` execution is intentionally gated until relocation, Linux exported
symbol binding and module init trampolines are complete.

## Ownership

PCI devices are never double-owned.

```text
PCI discovery
  -> native Tiramisu driver attempts attach
  -> if native attach succeeds: LDR does not claim
  -> if native attach fails or is unsupported: LDR may claim/expose device
```

For Intel Wi-Fi this produces:

```text
Linux net_device: wlan0
Tiramisu iface:   wlan0 / future wifi0 alias
Driver name:      linux-iwlwifi
```

## TNAI Packet Path

TX:

```text
Tiramisu socket
  -> Tiramisu TCP/UDP/IP/ARP stack
  -> TNAI netdev bridge
  -> Linux net_device ndo_start_xmit
  -> iwlwifi/iwlmvm
  -> hardware DMA
```

RX:

```text
hardware IRQ
  -> Linux IRQ shim
  -> iwlwifi NAPI/RX path
  -> Linux net_device receive
  -> TNAI RX callback
  -> Tiramisu IP stack
  -> native sockets
```

## Wi-Fi Control Path

```text
wifi scan
  -> userspace wifi applet
  -> SYS_WIFI_SCAN
  -> net_wifi_scan()
  -> native iwlwifi or LDR cfg80211 scan

wifi connect IFACE SSID PSK
  -> SYS_WIFI_CONNECT
  -> net_wifi_connect()
  -> cfg80211 connect
  -> WPA state machine / mac80211 / iwlmvm
  -> DHCP through native Tiramisu net stack

wifi disconnect IFACE
  -> SYS_WIFI_DISCONNECT
  -> net_wifi_disconnect()
  -> native iwlwifi disconnect or cfg80211 disconnect
```

## Kernel APIs

```c
void ldr_init(void);
int ldr_load_module(const char *name);
int ldr_load_module_image(const char *name, const void *image, size_t size);
int ldr_probe_pci_netdev(struct net_iface *iface, const struct pci_device *dev);
bool ldr_iface_is_linux_wifi(const struct net_iface *iface);
int ldr_wifi_scan(struct net_iface *iface);
int ldr_wifi_connect(struct net_iface *iface, const char *ssid, const char *passphrase);
int ldr_wifi_status(const struct net_iface *iface, struct wifi_status *out);
int ldr_wifi_scan_results(const struct net_iface *iface, struct wifi_ap *out, size_t max_aps);
int ldr_request_irq(uint8_t irq, ldr_irq_handler_t handler, void *ctx, const char *name);
int ldr_dma_map(void *cpu_addr, size_t size, bool streaming, struct ldr_dma_mapping *out);
int ldr_request_firmware(const char *name, struct ldr_firmware *out);
int ldr_cfg80211_disconnect(struct net_iface *iface);
```

## Pseudocode

Module load:

```c
ldr_load_module(name):
    mod = registry.find(name)
    deps = resolve_dependencies(mod)
    for dep in deps:
        ldr_load_module(dep)
    image = vfs_read("/lib/modules/" + name + ".ko")
    elf = parse_elf_relocatable(image)
    resolve_linux_symbols(elf)
    apply_relocations(elf)
    call_module_init(elf)
    mod.state = LOADED
```

PCI probe:

```c
ldr_probe_pci_netdev(iface, pci):
    if native_driver_owns(pci):
        return BUSY
    claim_device(pci)
    linux_pci_dev = make_linux_pci_dev(pci)
    redirect_irq(pci.irq, linux_irq_handler)
    expose_dma_ops(linux_pci_dev)
    call_matching_pci_driver_probe(linux_pci_dev)
```

cfg80211 scan:

```c
ldr_wifi_scan(iface):
    wiphy = tnai_to_wiphy(iface)
    request = build_cfg80211_scan_request()
    cfg80211_ops->scan(wiphy, request)
    wait_for_scan_done()
    copy_bss_list_to_tiramisu_wifi_ap()
```

## Debug Commands

```text
driver list
driver stats
linuxdrv logs
linuxdrv modules
linuxdrv stats
net trace
wifi debug
```

## Migration Plan

1. Finish `.ko` loader: relocatable ELF64, vermagic guard, module params,
   exported symbol table, init/exit trampolines.
2. Implement Linux core KAPI used by cfg80211/mac80211/iwlwifi: allocation,
   kthread, timers, mutex/spinlock/RCU stubs with Tiramisu semantics.
3. Implement PCI/IRQ/DMA bridge with IOMMU-safe ownership rules.
4. Bring up cfg80211 and mac80211 as Linux modules.
5. Bring up iwlwifi + iwlmvm and firmware request path.
6. Attach TNAI to native IP stack and DHCP.
7. Add suspend/resume recovery and reconnect.
8. Add i915 through DRM/KMS bridge for framebuffer and modesetting.

## Validation Matrix

```text
boot Intel laptop
driver list shows cfg80211/mac80211/iwlwifi/iwlmvm
linuxdrv stats shows claimed PCI Wi-Fi device
wifi scan returns APs
wifi connect obtains DHCP lease
ping google.com resolves DNS and sends ICMP
curl https://example.com uses native TCP/TLS through TNAI
suspend/resume triggers RECOVERY then RUNNING
```
