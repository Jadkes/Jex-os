#ifndef IRQ_H
#define IRQ_H

#include "isr.h"

typedef void (*irq_handler_t)(registers_t*);

void init_irq();
void register_interrupt_handler(uint8_t n, irq_handler_t handler);

#endif
