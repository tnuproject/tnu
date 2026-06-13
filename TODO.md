# TODO - Doom/Nano userspace TSS fix + FreeBSD-derived Wi-Fi metadata

## Step 1: Confirm current syscall/exec flow
- [x] Search for userspace exec/launch path (SYS_EXEC or similar) and locate where `arch_enter_user` is called.
- [x] Identify how syscall dispatch signals SYS_EXIT / return-to-shell today.

(confirmed: return-to-shell is currently hard-coded in `kernel/arch/x86_64/user.S` via `cmpq $7` special-case) 




## Step 2: Implement runtime GDT + 64-bit TSS (RSP0)

- [ ] Add runtime GDT/TSS descriptors after long-mode entry.
- [ ] Allocate kernel RSP0 stack for privilege transitions.
- [ ] Load TSS (LTR) and update descriptor loading order.

## Step 3: Fix ring-3 interrupt enabling
- [ ] Update `arch_enter_user` frame so IF is set for CPL3.
- [ ] Ensure syscall entry/exit keeps interrupts disabled until safe.


## Step 4: Replace hard-coded SYS_EXIT assembly special-case
- [x] Extend syscall dispatch to return an explicit disposition (resume vs return-to-kernel).
- [x] Modify `syscall_entry` assembly to branch on disposition (not syscall number == 7).


## Step 5: Wi-Fi metadata-only implementation

- [ ] Add Wi-Fi metadata tables mapping PCI IDs to families (iwn/iwm/iwlwifi) and dormant USB rtwn.
- [ ] Update `kernel/drivers/net/net.c` to register family names and unsupported capabilities.
- [ ] Ensure `wifi status/scan/connect` produce honest blockers.

## Step 6: Licensing + documentation
- [ ] Add/retain FreeBSD license headers in any adapted metadata files.
- [ ] Update README.md FreeBSD acknowledgement with credits and pointer to `freebsd-src/COPYRIGHT`.

## Step 7: Build + QEMU verification
- [ ] `make iso`
- [ ] QEMU smoke test: shell, uptime ticks, doom/nano progress beyond launch, syscall exit returns to shell.
- [ ] QEMU smoke test: wifi status/scan/connect messages.

