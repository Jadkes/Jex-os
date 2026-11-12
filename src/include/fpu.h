/**
 * @file fpu.h
 * @brief Per-task x87/SSE state, saved and restored across switches.
 *
 * Purpose: A task's floating-point registers are hardware state like any
 *          other register, but nothing in the scheduler used to save them.
 *          Two tasks that used float therefore overwrote each other's
 *          registers on every switch, silently.  This keeps one image per
 *          task so the x87 unit and MXCSR follow their owner.
 *
 * Design:  Eager save/restore at switch time, not lazy CR0.TS trapping.
 *          A 512-byte FXSAVE image is one cache-friendly memcpy per switch
 *          against a 100 Hz tick; a #NM handler would cost a trap plus the
 *          awkward question of what a kernel task does when it faults on
 *          its own FPU use inside an interrupt handler.  FNSAVE (108 bytes)
 *          covers CPUs without CPUID.FXSR.
 *
 *          Interrupt context must never touch float.  With eager switching
 *          the tick's use would be captured as the interrupted task's
 *          state, corrupting exactly the task it preempted.  The kernel is
 *          also built with -mno-80387, so kernel C cannot drift into it by
 *          accident; only a file explicitly allowed to use x87 can.
 *
 * Thread-safety: fpu_save()/fpu_restore() run in task_switch() with
 *          interrupts disabled, on the CPU that currently owns the unit.
 *          Each task's image is only ever touched by the CPU running it.
 */

#ifndef FPU_H
#define FPU_H

#include <stdint.h>

/* task.h owns the definition; a forward declaration keeps this header free
 * of the scheduler's dependencies (task.h is a heavy include). */
struct task;

/** @brief Bytes reserved per task: 512-byte FXSAVE image plus alignment. */
#define FPU_STATE_SLOT (512 + 15)

/**
 * @brief Detect the FPU, enable it, and capture the pristine task state.
 *
 * Runs as an early initcall on the BSP, before any task exists.
 */
void fpu_init_bsp(void);

/** @brief 1 when the CPU has an FPU and per-task state is being kept. */
int fpu_available(void);

/**
 * @brief Enable the FPU on the calling CPU (an AP).
 *
 * CR0/CR4 are per-CPU, so every core must do this for itself before the
 * first switch restores a task image onto it.
 */
void fpu_init_cpu(void);

/**
 * @brief Allocate and initialise a task's FPU image.
 *
 * @param t Task whose image to create.
 * @return 0 on success, -1 when the allocation failed.  Callers treat
 *         that as task-creation failure: a task without an image would
 *         share the raw unit with everyone else.
 */
int task_fpu_init(struct task* t);

/** @brief Free a task's FPU image.  Safe on a NULL/never-initialised task. */
void task_fpu_free(struct task* t);

/** @brief Save the calling CPU's FPU state into @p t's image. */
void fpu_save(struct task* t);

/** @brief Load @p t's image into the calling CPU's FPU. */
void fpu_restore(struct task* t);

#endif /* FPU_H */
