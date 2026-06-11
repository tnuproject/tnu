# Kernel Design

TNU is a monolithic x86_64 kernel. Drivers, filesystems, memory management,
process management, and syscall dispatch live in one privileged address space.
The code is still split by responsibility so readers can study each subsystem
without losing sight of the whole machine.

## Boot

`kernel/arch/x86_64/boot/start.S` contains the Multiboot2 header and long-mode
transition. It creates 2 MiB identity mappings for the first GiB, installs a
small GDT, and jumps to `arch_entry`.

## Interrupts

The kernel installs an IDT for CPU exceptions and legacy PIC IRQs. Timer IRQ0
updates scheduler ticks and keyboard IRQ1 feeds a line editor used by the boot
shell.

## Memory

The physical memory manager parses the Multiboot memory map and uses the
largest usable region above 1 MiB as a simple frame source. The virtual memory
manager documents and exposes the initial identity map. The heap is a bounded
kernel allocator used by VFS and subsystem setup.

## Processes

TNU 0.1 has a process table, PIDs, process states, users, working directories,
and file descriptors. It does not yet switch to ring 3. The scheduler is a
cooperative foundation with PIT accounting, intended to become preemptive once
user-mode context switching lands.

## Userspace Boundary

Userspace programs are built as freestanding ELF64 binaries with a TNU libc and
syscall ABI. The current boot shell is kernel-hosted so the system is usable
before the ring-3 loader is complete.
