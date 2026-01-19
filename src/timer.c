#include "timer.h"
#include "isr.h"
#include "irq.h"
#include "ports.h"

/* volatile is required so the compiler knows this value changes 
   outside of the normal program flow (in an interrupt) */
volatile uint32_t system_ticks = 0;

void timer_callback(registers_t *regs)
{
    system_ticks++;
    (void)regs;
}

void init_timer(uint32_t frequency)
{
    register_interrupt_handler(0, timer_callback);

    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0x36);
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)( (divisor>>8) & 0xFF );
    outb(0x40, l);
    outb(0x40, h);
}

void sleep(uint32_t ms)
{
    uint32_t start_ticks = system_ticks;
    uint32_t ticks_to_wait = ms / 10;
    
    /* We MUST enable interrupts here because sleep is often called 
       from inside another interrupt (like the keyboard). 
       If interrupts stay disabled, the timer never ticks! */
    __asm__ volatile("sti");

    while(system_ticks < start_ticks + ticks_to_wait)
    {
        /* Wait for the next interrupt */
        __asm__ volatile("hlt");
    }
}

uint32_t get_ticks()
{
    return system_ticks;
}