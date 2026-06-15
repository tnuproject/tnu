/*
 * Minimal ioctl macro definitions for the TNU kernel.
 * The driver code uses the classic _IO, _IOR, _IOW macros to generate
 * ioctl request numbers. We provide simple implementations compatible
 * with the userspace <sys/ioctl.h> definitions.
 */

#ifndef TNU_IOCTL_H
#define TNU_IOCTL_H

/*
 * The layout mirrors the Linux _IOC macros but is simplified. The exact
 * numeric values are not critical for the current stub implementation –
 * they just need to be distinct and fit in an unsigned long.
 */

/* Direction bits */
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

/* Encode an ioctl request number.
 *   dir  – direction (none, read, write)
 *   type – magic number (usually a character)
 *   nr   – command number
 *   size – size of the argument type (ignored for stub)
 */
#define _IOC(dir,type,nr,size) \
    (((dir)  << 30) | ((type) << 8) | (nr))

/* Helper macros */
#define _IO(type,nr)            _IOC(_IOC_NONE,  (type), (nr), 0)
#define _IOR(type,nr,datatype)  _IOC(_IOC_READ,  (type), (nr), sizeof(datatype))
#define _IOW(type,nr,datatype)  _IOC(_IOC_WRITE, (type), (nr), sizeof(datatype))
#define _IOWR(type,nr,datatype) _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(datatype))

#endif /* TNU_IOCTL_H */
