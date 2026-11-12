/**
 * @file task.h
 * @brief Multi-tasking and process management.
 *
 * Defines the Process Control Block (PCB) and task states.
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "paging.h"
#include "isr.h"
#include "smp.h"

/**
 * @enum task_state_t
 * @brief Current execution state of a task.
 */
typedef enum {
    STATE_READY,        /**< Task is waiting to be scheduled. */
    STATE_RUNNING,      /**< Task is currently executing. */
    STATE_SLEEPING,     /**< Task is waiting for a timer or event. */
    STATE_BLOCKED,      /**< Task is parked on a mutex/completion wait list. */
    STATE_ZOMBIE        /**< Task has finished but not yet cleaned up. */
} task_state_t;

/**
 * @struct task
 * @brief Process Control Block (PCB).
 *
 * Holds all information needed to manage and switch between processes.
 */
typedef struct task {
    int id;                             /**< Unique Process ID (PID). */
    uint32_t esp, ebp;                  /**< Stack and base pointers for context switching. */
    uint32_t eip;                       /**< Instruction pointer for context switching. */
    page_directory_t* page_directory;   /**< Virtual memory space for this task. */
    uint32_t kstack;                    /**< Kernel stack base for this task. */
    void* fpu_state;                    /**< Per-task x87/SSE image, aligned on use.
                                             See fpu.h; NULL only when the CPU
                                             reports no FPU. */
    task_state_t state;                 /**< Current execution state. */
    volatile int on_cpu;                /**< CPU index currently executing
                                             this task, or -1 if unclaimed.
                                             The scheduler ring is global;
                                             without a claim two CPUs can
                                             pick the same RUNNING task and
                                             interleave on one kstack. */
    char name[16];                      /**< Human-readable task name */
    uint32_t cpu_ticks;                 /**< CPU ticks consumed (for top command). */
    struct task* next;                  /**< Next task in the linked scheduler list. */
    struct task* wait_next;             /**< FIFO linkage while blocked on a sync object.
                                             The scheduler list keeps the task linked;
                                             this chain belongs to the lock's waiter queue. */

    int parent_pid;                     /**< PID of the parent process (0 for init). */
    int exit_code;                      /**< Exit code set by task_exit (valid in ZOMBIE state). */

    /* Signal handling */
    uint32_t signal_pending;            /**< Bitmask: pending signals (set by kill, cleared by delivery) */
    uint32_t signal_blocked;            /**< Bitmask: blocked signals */
    uint32_t isr_depth;                 /**< IRQ nesting depth for THIS task.
                                             A global counter lies across
                                             preemption: the victim stays
                                             frozen inside irq_handler while
                                             other tasks run, so atomic-context
                                             checks must be per-task. */
    void*    signal_handlers[32];       /**< Per-signal handlers (NULL = SIG_DFL) */
} task_t;

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGKILL   9
#define SIGTERM  15
#define SIGCHLD  17

/* Signal actions */
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)
#define SIG_ERR ((void*)-1)

/**
 * @brief Initialize the multitasking system.
 */
void init_tasking();

/**
 * @brief Force a context switch to the next ready task.
 */
void task_switch();

/**
 * @brief Park the calling task (BLOCKED) and yield once.
 * The caller must already be enqueued on a wait list under the sync
 * object's own lock.  Returns after being woken; re-check conditions.
 */
void task_block_current(void);

/**
 * @brief Wake a blocked task so the scheduler can run it again.
 * Safe from ISR or any CPU; no-op for non-blocked tasks.
 */
void task_wake(task_t* t);

/* Switch-starvation watchdog (debug flavors): timer ticks call _tick,
 * every completed scheduler pick calls _reset.  A trip dumps all task
 * states -- the only witness when everything is parked, since nothing
 * spins and no STUCKDUMP can fire. */
void sched_watchdog_tick(void);
void sched_watchdog_reset(void);

/**
 * @brief Create a new process by duplicating the current one.
 *
 * @param regs The user-mode register state from the syscall entry, used to
 *             build the child's synthetic iret frame.
 * @return 0 in the child, child PID in the parent, -1 on error.
 */
int fork(registers_t* regs);

/**
 * @brief Create a new kernel task that executes func(arg).
 *
 * Unlike fork(), this does not take a user-mode registers_t frame and does not
 * build a synthetic iret frame.  The child starts at a trampoline
 * that calls func(arg) then task_exit(0).
 *
 * @param func Function to execute in the child (takes one void* arg).
 * @param arg  Opaque argument to pass to func.
 * @return Child PID on success, -1 on allocation failure.
 */
int kernel_fork(void (*func)(void*), void* arg);

/**
 * kernel_fork_on - spawn a kernel task running func(arg) on a specific CPU.
 * @func: entry point; returning from it exits the task.
 * @arg: passed through to func.
 * @cpu: target CPU index, or -1 to place by load.
 *
 * @return child pid, or negative errno.
 */
int kernel_fork_on(void (*func)(void*), void* arg, int cpu);

/**
 * @brief Get the PID of the currently running task.
 */
int getpid();

/**
 * @brief Exit the current task with an exit code and mark it as a zombie.
 * @param exit_code The exit status to pass to the parent via waitpid().
 */
void _task_exit(int exit_code);

/**
 * @brief Exit the current task and mark it as a zombie.
 */
void task_exit();

/**
 * @brief Wait for a child process to exit.
 * @param pid Child PID, or -1 for any child.
 * @param status If non-NULL, stores the exit code.
 * @param options Flags (must be 0 for now).
 * @return PID of reaped child, or -1 on error.
 */
int waitpid(int pid, int* status, int options);

/**
 * @brief List all active tasks to the terminal (for debugging).
 */
void task_list();

/**
 * @brief Mark a task by PID as a zombie.
 * @param pid The PID to kill.
 * @return 0 on success, -1 if not found.
 */
int task_kill(int pid);

/* Signal syscalls */
void* sys_signal(int sig, void* handler);
int   sys_kill(int pid, int sig);

/* PID of the shell's foreground child, or 0 when the shell owns the console.
 * The keyboard ISR turns Ctrl+C into SIGINT for this task. */
extern volatile int fg_child_pid;

/* Find a task by PID across every CPU ready queue, NULL if absent. */
task_t* task_from_pid(int pid);

/* Serialize read-side scans of any CPU's ready queue from outside
 * task.c (ps/top/cpuinfo).  Not nestable with itself. */
void tasks_lock_irqsave(uint32_t* flags);
void tasks_unlock_irqrestore(uint32_t flags);

/* Create the idle task for an AP before it is woken (BSP-only call). */
void task_spawn_idle(int cpu_index);

/**
 * @brief Per-CPU current-task and ready-queue accessors.
 *
 * Each CPU has its own ready queue and current task, stored in cpus[].
 * These inline helpers provide convenient per-CPU access.  On the BSP with
 * a single core they return the same values the old globals did.
 */
static inline task_t* cur_task(void) {
    return (task_t*)cpus[cpu_id()].current_task;
}
static inline task_t* rdy_queue(void) {
    return (task_t*)cpus[cpu_id()].ready_queue;
}
static inline void set_cur_task(task_t* t) {
    cpus[cpu_id()].current_task = (struct task*)t;
}
static inline void set_rdy_queue(task_t* q) {
    cpus[cpu_id()].ready_queue = (struct task*)q;
}

#endif // TASK_H
