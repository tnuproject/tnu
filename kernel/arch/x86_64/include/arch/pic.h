#ifndef TNU_ARCH_PIC_H
#define TNU_ARCH_PIC_H

#include <tnu/types.h>

void pic_remap(uint8_t master_offset, uint8_t slave_offset);
void pic_set_mask(uint8_t irq);
void pic_clear_mask(uint8_t irq);
void pic_send_eoi(uint8_t irq);

#endif
