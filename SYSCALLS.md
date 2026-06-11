# Syscalls

TNU defines a Unix-like syscall table. The in-kernel dispatcher is the active
ABI contract used by the boot shell and freestanding userspace objects.

| Number | Name |
| ---: | --- |
| 0 | read |
| 1 | write |
| 2 | open |
| 3 | close |
| 4 | spawn |
| 5 | exec |
| 6 | wait |
| 7 | exit |
| 8 | getpid |
| 9 | chdir |
| 10 | getcwd |
| 11 | mkdir |
| 12 | unlink |
| 13 | stat |
| 14 | chmod |
| 15 | chown |
| 16 | getuid |
| 17 | getgid |

The userspace libc places syscall numbers in `rax` and arguments in the usual
x86_64 register order.
