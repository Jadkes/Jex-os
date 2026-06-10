#ifndef GDB_STUB_H
#define GDB_STUB_H

#include <stdint.h>
#include "isr.h"

/**
 * gdb_stub_handler - Entry point for int3 (breakpoint) handler.
 *
 * Called from the IDT's interrupt 3 handler with a pointer to the saved
 * register frame.  Restores the breakpoint byte, processes GDB remote
 * protocol commands over COM1, and modifies the register frame as needed
 * (eip, eflags) before returning.
 *
 * @regs  Pointer to the register save area on the interrupt stack.
 *        Modifications to this frame are reflected by iret.
 */
void gdb_stub_handler(registers_t *regs);

/**
 * gdb_stub_handle_trace - Handle debug exception (int 1) from single-step.
 *
 * Called by the isr handler when int 1 fires with the Trap Flag set.
 * If stepping_over is active (we stepped past a breakpoint for 'c'),
 * the function silently continues.  Otherwise it calls gdb_stub_handler
 * to re-enter the command loop for user-requested single-steps.
 *
 * @regs  Pointer to register frame (may be modified for step-over).
 */
void gdb_stub_handle_trace(registers_t *regs);

/**
 * gdb_breakpoint - Trigger a breakpoint and enter GDB stub.
 *
 * Insert into any kernel code to drop into the debugger.  Requires a
 * GDB client to be connected to the serial port.
 */
static inline void gdb_breakpoint(void)
{
    __asm__ volatile("int3");
}

#endif /* GDB_STUB_H */
