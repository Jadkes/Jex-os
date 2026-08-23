/**
 * @file task.c
 * @brief Preemptive multitasking and process management.
 *
 * Implements a simple round-robin scheduler and task switching logic.
 */

#include "task.h"
#include "kheap.h"
#include "pmm.h"

#include "paging.h"
#include "init.h"
#include "string.h"
#include "gdt.h"
#include "panic.h"
#include "isr.h"
#include "ports.h"
#include "spinlock.h"
#include "kernel/printk.h"
#include "kernel/backtrace.h"
#include <jexos/errno.h>

extern void log_serial(const char* str);
extern void log_hex_serial(uint32_t n);

/*
 * Cross-CPU mutual exclusion for everything that touches any ready
 * queue: fork/exit appends, waitpid reaps, kill/pid lookups, ps-style
 * scans, and the scheduler's own queue walk in task_switch().  A local
 * cli cannot do this job once two cores mutate the lists concurrently.
 */
static spinlock_t tasklist_lock = SPINLOCK_INIT;

/* Hands out PIDs atomically across CPUs */
static spinlock_t pid_lock = SPINLOCK_INIT;

/**
 * @brief Serialize cross-CPU task-queue scans for code outside task.c
 *        (shell ps/top/cpuinfo).  Nesting is NOT supported.
 */
void tasks_lock(void)   { spin_lock(&tasklist_lock); }
void tasks_unlock(void) { spin_unlock(&tasklist_lock); }

/**
 * @brief Pick the online CPU with the shortest ready queue.
 *
 * Called with tasklist_lock held so queue lengths are stable.
 * @return CPU index for a new task.
 */
static int pick_cpu_by_load(void)
{
    int best = 0;
    int best_len = -1;
    for (int i = 0; i < cpu_count; i++) {
        /* cpus[0].online is never set (only APs signal it) */
        if (i != 0 && !cpus[i].online) continue;
        int len = 0;
        for (task_t* t = (task_t*)cpus[i].ready_queue; t; t = t->next) len++;
        if (best_len < 0 || len < best_len) { best_len = len; best = i; }
    }
    return best;
}

/**
 * @brief Append a task to the tail of a CPU's ready queue.
 * Caller holds tasklist_lock.
 */
static void queue_append(int cpu_index, task_t* t)
{
    task_t* q = (task_t*)cpus[cpu_index].ready_queue;
    if (!q) {
        cpus[cpu_index].ready_queue = (struct task*)t;
        return;
    }
    while (q->next) q = q->next;
    q->next = t;
}


extern page_directory_t kernel_directory;
extern uint32_t read_eip();
extern void terminal_writestring(const char* s);
extern void terminal_putchar(char c);
extern void int_to_string(int n, char* str);

/* Declared in kernel_fork_trampoline asm label below */
extern void kernel_fork_trampoline(void);

/**
 * @brief Per-CPU ready queues and current task.
 *
 * Stored in cpus[] and accessed via cur_task() / rdy_queue() inline helpers.
 * Each CPU owns its own queue and never touches another CPU's.
 */

static int next_pid = 1;

/**
 * @brief Per-CPU idle body — sleeps until the LAPIC timer preempts.
 *
 * Runs as a normal ring-0 task on its CPU's queue; the periodic tick
 * saves this context like any other and gives the CPU to real work.
 */
static void idle_loop(void)
{
    for (;;)
        __asm__ volatile("sti; hlt");
}

/**
 * @brief Initialize multitasking.
 * Creates the first task (Kernel/Shell) and starts the scheduler.
 */
void init_tasking() {
    __asm__ volatile("cli");

    set_cur_task((task_t*)kmalloc(sizeof(task_t)));
    set_rdy_queue(cur_task());
    cur_task()->id = next_pid++;
    cur_task()->esp = cur_task()->ebp = 0;
    cur_task()->eip = 0;
    cur_task()->page_directory = &kernel_directory;
    cur_task()->kstack = alloc_kernel_stack();
    cur_task()->cpu_ticks = 0;
    cur_task()->next = NULL;
    cur_task()->state = STATE_RUNNING;
    cur_task()->parent_pid = 0;
    cur_task()->exit_code = 0;
    cur_task()->signal_pending = 0;
    cur_task()->signal_blocked = 0;
    for (int i = 0; i < 32; i++)
        cur_task()->signal_handlers[i] = NULL;
    strcpy((char*)cur_task()->name, "shell");

    __asm__ volatile("sti");
}

/**
 * @brief Deliver pending signals to a task before it runs.
 *
 * Checks signal_pending (minus blocked signals) and handles delivery:
 * - SIGKILL: immediate termination
 * - SIG_DFL with terminate default: set STATE_ZOMBIE
 * - SIG_IGN, SIG_DFL with ignore default (SIGCHLD): skip
 * - Handler function: sets up signal trampoline on user stack
 *
 * @param task The task to deliver signals to.
 * @return 1 if task was terminated (don't schedule), 0 otherwise.
 */
static int deliver_signals(task_t* task)
{
    uint32_t pending = task->signal_pending & ~task->signal_blocked;
    if (!pending) return 0;

    /* Find lowest pending signal number */
    int sig = __builtin_ctz(pending);  /* ffs: returns 0-31, 0 = bit 0 */

    /* Clear the pending bit */
    task->signal_pending &= ~(1 << sig);

    /* SIGKILL — immediate termination, cannot be caught or ignored */
    if (sig == SIGKILL) {
        task->state = STATE_ZOMBIE;
        return 1;
    }

    /* Look up handler */
    void* handler = task->signal_handlers[sig];

    if (handler == SIG_IGN) {
        /* Ignored — continue normally */
        return 0;
    }

    if (handler == SIG_DFL || handler == NULL) {
        /* Default action */
        if (sig == SIGCHLD) {
            /* SIGCHLD default is ignore */
            return 0;
        }
        /* All others: terminate */
        task->state = STATE_ZOMBIE;
        return 1;
    }

    /* User handler function — for now, just note it (user-mode not fully implemented)
     * In a full implementation we'd push a trampoline frame on the user stack.
     * For this kernel-stage: skip user handlers silently for now.
     */
    (void)handler;
    return 0;
}

/**
 * @brief Switch execution to the next ready task.
 * Saves the current context and loads the next one using assembly magic.
 */
void task_switch() {
    task_t* ct = cur_task();
    if (!ct) return;

    uint32_t esp, ebp, eip;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    /* Magic return for the next time we are switched TO */
    eip = read_eip();
    if (eip == 0x12345) return;

    ct->eip = eip;
    ct->esp = esp;
    ct->ebp = ebp;

    /*
     * Pick the next task under the global lock — other CPUs mutate this
     * queue concurrently.  The lock is dropped before the context jump:
     * the choice below is already final, and holding a spinlock across a
     * switch would freeze whichever CPU preempts us mid-jump.
     */
    uint32_t tl_flags;
    spin_lock_irqsave(&tasklist_lock, &tl_flags);

    task_t* start = ct;
    ct = ct->next;
    if (!ct) ct = rdy_queue();

    /* Skip zombie tasks — they stay around for the parent to reap */
    {
        while (ct->state == STATE_ZOMBIE) {
            ct = ct->next;
            if (!ct) ct = rdy_queue();
            /* If we wrapped around and the ONLY non-zombie task is start
             * itself, the while-loop check re-evaluates start's state on the
             * next iteration.  But if start is ALSO zombie, every task is
             * zombie and there's nothing to schedule. */
            if (ct == start && ct->state == STATE_ZOMBIE) {
                /* All tasks are zombies — nothing to run */
                spin_unlock_irqrestore(&tasklist_lock, tl_flags);
                for (;;) asm volatile("hlt");
            }
        }
    }

    /* Deliver any pending signals before the task runs */
    while (deliver_signals(ct)) {
        /* ct was terminated: leave the zombie LINKED so waitpid() can
         * find and reap it (unlinking here made killed children vanish —
         * unreapable, leaking their stack and task struct).  Hop to the
         * next non-zombie candidate instead. */
        task_t* scan_from = ct;
        do {
            ct = ct->next;
            if (!ct) ct = rdy_queue();
        } while (ct->state == STATE_ZOMBIE && ct != scan_from);
        if (ct->state == STATE_ZOMBIE)
            panic_assert("All tasks terminated by signals", __FILE__, __LINE__);
    }

    spin_unlock_irqrestore(&tasklist_lock, tl_flags);

    /* Persist the selected task */
    set_cur_task(ct);

    esp = ct->esp;
    ebp = ct->ebp;
    eip = ct->eip;

    if (ct->kstack) {
        set_kernel_stack(ct->kstack + KSTACK_SIZE);
    }


    __asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "b"(eip), "r"(esp), "r"(ebp), "r"(ct->page_directory) : "eax");
}

/**
 * @struct fork_save_t
 * @brief Saved user-mode register state for fork child entry.
 *
 * Placed on the child's kernel stack by fork(), then read by
 * fork_child_entry() to restore user registers and iret to user mode.
 */
typedef struct {
    uint32_t ecx, edx, ebx, esi, edi, ebp;  /**< General registers preserved across fork */
    uint32_t eip, useresp, eflags;           /**< Values for the iret frame */
} fork_save_t;

/**
 * @brief Fork child entry point — called via kernel_fork_trampoline.
 *
 * Restores the parent's user-mode register state from the fork_save_t,
 * sets EAX = 0 (fork's child return value), builds a standard iret frame
 * (same pattern as jump_to_user_mode in usermode.s), and transitions to
 * user mode.  All registers except EAX are preserved, so fork() looks
 * transparent to the child.
 *
 * @param arg Pointer to a fork_save_t on the child's kernel stack.
 */
static void fork_child_entry(void* arg) {
    fork_save_t* save = (fork_save_t*)arg;


    /* Load into C locals — iret-frame values use "m" constraint so they
     * live in memory.  This guarantees that restoring all GPRs below
     * cannot clobber eip / useresp / eflags before we push them. */
    uint32_t eip     = save->eip;
    uint32_t uesp    = save->useresp;
    uint32_t eflags  = save->eflags | 0x200;   /* ensure IF = 1 */
    uint32_t ecx_val = save->ecx;
    uint32_t edx_val = save->edx;
    uint32_t ebx_val = save->ebx;
    uint32_t esi_val = save->esi;
    uint32_t edi_val = save->edi;
    uint32_t ebp_val = save->ebp;

    __asm__ volatile(
        /* Restore user registers — everything EXCEPT EBP, which we defer.
         * All operands use "m" (kernel-stack memory via EBP) so the register
         * clobber list can be exhaustive.  GCC must not assign ANY input to
         * a register we clobber before reading it — the "m" constraint avoids
         * this problem entirely. */
        "mov  %3, %%ecx\n"
        "mov  %4, %%edx\n"
        "mov  %5, %%ebx\n"
        "mov  %6, %%esi\n"
        "mov  %7, %%edi\n"

        /* Switch to user-mode segment selectors */
        "mov  $0x23, %%ax\n"
        "mov  %%ax, %%ds\n"
        "mov  %%ax, %%es\n"
        "mov  %%ax, %%fs\n"
        "mov  %%ax, %%gs\n"

        /* EAX = 0 — fork child return value */
        "xor  %%eax, %%eax\n"

        /* Build 5-item iret frame: SS, ESP, EFLAGS, CS, EIP
         * EBP is still the kernel frame pointer here — "m" reads are valid. */
        "push $0x23\n"
        "push %1\n"          /* user ESP */
        "push %2\n"          /* EFLAGS (IF already set) */
        "push $0x1B\n"       /* user CS */
        "push %0\n"          /* EIP */

        /* NOW restore EBP — after all "m" references have consumed their values.
         * iret does not use EBP, so the user's value is correct at this point. */
        "mov  %8, %%ebp\n"

        "iret\n"
        :
        : "m"(eip), "m"(uesp), "m"(eflags),
          "m"(ecx_val), "m"(edx_val), "m"(ebx_val),
          "m"(esi_val), "m"(edi_val), "m"(ebp_val)
        : "eax", "ecx", "edx", "ebx", "esi", "edi", "memory"
    );

    /* Unreachable — iret never returns */
}

/**
 * @brief Create a new process (child) by duplicating the current one (parent).
 *
 * The child starts via kernel_fork_trampoline → fork_child_entry(), which
 * restores the parent's register state and iret's to user mode with EAX = 0.
 * This reuses the same proven iret mechanism as jump_to_user_mode (usermode.s)
 * instead of a fragile synthetic pusha frame.
 *
 * @param regs The user-mode registers_t captured by the ISR stub on int 0x80 entry.
 * @return Child PID for the parent, -1 on error. (The child never returns from this call.)
 */
int fork(registers_t* regs) {
    task_t* parent = cur_task();

    /* Allocate task_t via kmalloc so waitpid's kfree() matches.
     * Previously used pmm_alloc_block() which kfree() cannot free
     * (ptr_to_slab sees task_t.id as "magic" — never matches). */
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child)
        return -ENOMEM;

    /* Clone the page directory */
    page_directory_t* new_dir = clone_page_directory(parent->page_directory);
    if (!new_dir) {
        kfree(child);
        return -ENOMEM;
    }

    uint32_t flags;
    spin_lock_irqsave(&pid_lock, &flags);
    child->id = next_pid++;
    spin_unlock_irqrestore(&pid_lock, flags);
    child->page_directory = new_dir;
    child->cpu_ticks = 0;
    child->state = STATE_READY;
    child->next = NULL;
    child->parent_pid = parent->id;
    child->exit_code = 0;
    strcpy(child->name, parent->name);
    child->signal_pending = 0;
    child->signal_blocked = parent->signal_blocked;
    for (int i = 0; i < 32; i++)
        child->signal_handlers[i] = parent->signal_handlers[i];

    /* Allocate kernel stack for the child via PDE 1023 — no identity collision */
    child->kstack = alloc_kernel_stack();
    if (!child->kstack) {
        kfree(child);
        return -ENOMEM;
    }

    /*
     * Place a fork_save_t on the child's kernel stack (at a HIGHER address
     * than the trampoline frame), then push func/arg for kernel_fork_trampoline.
     *
     * Layout (low → high address):
     *   [func]    ← child->esp = kernel_fork_trampoline pops into EBX
     *   [arg]     ← trampoline pops into EAX, pushes back for call
     *   [save ]   ┐   fork_save_t (9 × 4 = 36 bytes)
     *   [...]    ┘   referenced by save pointer above
     *
     * NOTE: The child's kernel stack lives in PDE 1023 (shared page table,
     * visible from all CR3 values).  Writes via child->kstack reach the
     * correct physical pages directly — no CR3 switch needed.
     *
     * kernel_fork_trampoline: pop EBX(=func) / pop EAX(=save) / push EAX / call *EBX
     */
    uint32_t* cp = (uint32_t*)(child->kstack + KSTACK_SIZE);

    /* Write the fork_save_t */
    cp -= sizeof(fork_save_t) / 4;
    fork_save_t* save = (fork_save_t*)cp;
    save->ecx     = regs->ecx;
    save->edx     = regs->edx;
    save->ebx     = regs->ebx;
    save->esi     = regs->esi;
    save->edi     = regs->edi;
    save->ebp     = regs->ebp;
    save->eip     = regs->eip;
    save->useresp = regs->useresp;
    save->eflags  = regs->eflags;

    /* Push func and arg for kernel_fork_trampoline */
    *--cp = (uint32_t)save;              /* arg (popped second, passed to func) */
    *--cp = (uint32_t)fork_child_entry;  /* func (popped first, called) */

    child->esp = (uint32_t)cp;
    child->ebp = (uint32_t)cp;
    child->eip = (uint32_t)kernel_fork_trampoline;


    /* Place the child on the least-loaded CPU and link it in */
    spin_lock_irqsave(&tasklist_lock, &flags);
    queue_append(pick_cpu_by_load(), child);
    spin_unlock_irqrestore(&tasklist_lock, flags);

    return child->id;
}

/**
 * @brief Assembly trampoline for kernel_fork'd tasks.
 *
 * Pops function pointer into EBX and argument into EAX from the child's
 * initial stack (pushed by kernel_fork), calls func(arg), then calls
 * task_exit when func returns.
 */
__asm__(
    ".globl kernel_fork_trampoline\n"
    "kernel_fork_trampoline:\n"
    "    pop %ebx\n"
    "    pop %eax\n"
    "    push %eax\n"
    "    call *%ebx\n"
    "    push $0\n"
    "    call task_exit\n"
);

/**
 * @brief Create a new kernel task that starts executing func(arg).
 *
 * Unlike fork(), this does not copy the parent's user-mode register state.
 * The child begins at kernel_fork_trampoline,
 * which calls func(arg) and then task_exit(0) when func returns.
 *
 * @param func Function to execute in the child (takes one void* arg).
 * @param arg  Opaque argument to pass to func.
 * @return Child PID on success, -1 on allocation failure.
 */
/*
 * kernel_fork_on - Create a new kernel task, targetting a specific CPU.
 *
 * @func: Function to execute.
 * @arg:  Argument to pass to @func.
 * @cpu:  Target logical CPU number, or -1 to place by load.
 *
 * Cross-CPU queueing is safe now that every core runs its own scheduler
 * under tasklist_lock; waitpid() scans all queues when reaping.
 *
 * Return: child PID on success, or negative errno.
 */
int kernel_fork_on(void (*func)(void*), void* arg, int cpu)
{
    int place_by_load = (cpu < 0 || cpu >= cpu_count);

    /*
     * Allocate and clone with interrupts ENABLED.
     *
     * In the old code the cli covered clone_page_directory too, creating a
     * multi-millisecond interrupt hole that could cause timer IRQ starvation
     * in the parent's subsequent waitpid yield loop.  No interrupt handler
     * touches the PMM bitmap or the page tables during fork, so it is safe
     * to let the timer fire through the clone.
     */
    task_t* parent = cur_task();

    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child)
        return -ENOMEM;

    /* Clone the page directory */
    page_directory_t* new_dir = clone_page_directory(parent->page_directory);
    if (!new_dir) {
        kfree(child);
        return -ENOMEM;
    }

    uint32_t flags;
    spin_lock_irqsave(&pid_lock, &flags);
    child->id = next_pid++;
    spin_unlock_irqrestore(&pid_lock, flags);

    child->page_directory = new_dir;
    child->cpu_ticks = 0;
    child->state = STATE_READY;
    child->next = NULL;
    child->parent_pid = parent->id;
    child->exit_code = 0;
    strcpy(child->name, parent->name);
    child->signal_pending = 0;
    child->signal_blocked = parent->signal_blocked;
    for (int i = 0; i < 32; i++)
        child->signal_handlers[i] = parent->signal_handlers[i];

    child->kstack = alloc_kernel_stack();
    if (!child->kstack) {
        kfree(child);
        return -ENOMEM;
    }

    /* Set up the child's initial stack for the trampoline. */
    uint32_t* cp = (uint32_t*)(child->kstack + KSTACK_SIZE);
    *--cp = (uint32_t)arg;
    *--cp = (uint32_t)func;

    child->esp = (uint32_t)cp;
    child->ebp = (uint32_t)cp;
    child->eip = (uint32_t)kernel_fork_trampoline;

    spin_lock_irqsave(&tasklist_lock, &flags);
    queue_append(place_by_load ? pick_cpu_by_load() : cpu, child);
    spin_unlock_irqrestore(&tasklist_lock, flags);

    return child->id;
}

int kernel_fork(void (*func)(void*), void* arg)
{
    return kernel_fork_on(func, arg, -1);
}

/**
 * @brief Exit the current task with an exit code.
 *
 * Sets state to ZOMBIE but stays in the ready queue so the parent
 * can find and reap it via waitpid(). The scheduler skips zombies.
 */
void _task_exit(int exit_code) {
    task_t* dying = cur_task();

    /* PID 1 (shell/init) cannot be killed this way */
    if (dying->id == 1)
        return;

    log_serial("[EXIT] pid=");
    log_hex_serial(dying->id);
    log_serial(" code=");
    log_hex_serial(exit_code);
    log_serial("\n");

    uint32_t flags;
    spin_lock_irqsave(&tasklist_lock, &flags);

    /*
     * Reparent orphans across EVERY cpu's queue — a child forked onto
     * another core must still find its way to init (PID 1), or it can
     * never be reaped.
     */
    for (int i = 0; i < cpu_count; i++) {
        for (task_t* scan = (task_t*)cpus[i].ready_queue; scan; scan = scan->next) {
            if (scan->parent_pid == dying->id) {
                scan->parent_pid = 1;
                log_serial("[REPARENT] pid=");
                log_hex_serial(scan->id);
                log_serial(" to init\n");
            }
        }
    }

    dying->exit_code = exit_code;
    dying->state = STATE_ZOMBIE;

    spin_unlock_irqrestore(&tasklist_lock, flags);

    task_switch();
}

/**
 * @brief Mark the current task as finished with exit code 0.
 */
void task_exit() {
    _task_exit(0);
}

/**
 * @brief Wait for a child process to exit.
 *
 * Scans the ready queue for a zombie child matching @pid.
 * If no zombie yet and children exist, busy-waits yielding to the scheduler.
 * Reaps the child: frees its kernel stack and task_t struct.
 *
 * @param pid Child PID to wait for, or -1 for any child.
 * @param status If non-NULL, stores the child's exit code here.
 * @param options Ignored (must be 0).
 * @return PID of the reaped child, or -1 on error.
 */
static void reap_orphan_zombies(void);

int waitpid(int pid, int* status, int options) {
    (void)options;

    task_t* self = cur_task();

    while (1) {
        uint32_t flags;
        spin_lock_irqsave(&tasklist_lock, &flags);

        /*
         * Scan every CPU's queue for a zombie child — children may have
         * been placed on another core by pick_cpu_by_load().
         */
        for (int i = 0; i < cpu_count; i++) {
            task_t* prev = NULL;
            task_t* scan = (task_t*)cpus[i].ready_queue;
            while (scan) {
                if (scan->state == STATE_ZOMBIE &&
                    scan->parent_pid == self->id &&
                    (pid == -1 || scan->id == pid)) {
                    if (status)
                        *status = scan->exit_code;
                    int child_pid = scan->id;
                    log_serial("[WAITPID] reaping pid=");
                    log_hex_serial(child_pid);
                    log_serial(" by pid=");
                    log_hex_serial(self->id);
                    log_serial(" code=");
                    log_hex_serial(scan->exit_code);
                    log_serial("\n");

                    /* Free kernel stack — PDE 1023 shared page table */
                    if (scan->kstack)
                        free_kernel_stack((uint32_t)scan->kstack);

                    /* TODO(jadkes): Free the child's page directory and all
                     * cloned page tables/pages.  Currently leaked — each
                     * fork() or kernel_fork() that exits leaks ~10+ physical
                     * pages.  Needs a full teardown walking all user PDEs. */

                    /* Unlink from whichever queue held it */
                    if (prev)
                        prev->next = scan->next;
                    else
                        cpus[i].ready_queue = (struct task*)scan->next;

                    kfree(scan);

                    /* Reap orphaned zombies reparented to us (PID 1/init):
                     * a grandchild that outlived its parent has nobody else. */
                    if (self->id == 1)
                        reap_orphan_zombies();

                    spin_unlock_irqrestore(&tasklist_lock, flags);
                    return child_pid;
                }
                prev = scan;
                scan = scan->next;
            }
        }

        /* Check if any children exist at all */
        {
            int any = 0;
            for (int i = 0; i < cpu_count && !any; i++) {
                for (task_t* scan = (task_t*)cpus[i].ready_queue; scan; scan = scan->next) {
                    if (scan->parent_pid == self->id) {
                        any = 1;
                        break;
                    }
                }
            }
            if (!any) {
                spin_unlock_irqrestore(&tasklist_lock, flags);
                return -ECHILD;  /* No children */
            }
        }

        spin_unlock_irqrestore(&tasklist_lock, flags);

        /* Children exist but none exited yet — pause and let the timers
         * preempt us; no manual sti/cli juggling needed any more. */
        __asm__ volatile("pause");
    }
}

/**
 * @brief Reap zombie tasks reparented to init (PID 1) on any CPU.
 * Caller holds tasklist_lock.
 */
static void reap_orphan_zombies(void)
{
    for (int i = 0; i < cpu_count; i++) {
        task_t* p = NULL;
        task_t* s = (task_t*)cpus[i].ready_queue;
        while (s) {
            if (s->state == STATE_ZOMBIE &&
                s->parent_pid == 1 && s->id != 1) {
                task_t* dead = s;
                if (p)
                    p->next = dead->next;
                else
                    cpus[i].ready_queue = (struct task*)dead->next;
                s = dead->next;
                if (dead->kstack)
                    free_kernel_stack((uint32_t)dead->kstack);
                kfree(dead);
            } else {
                p = s;
                s = s->next;
            }
        }
    }
}

void _waitpid_done(void) {} /* marker for breakpoint */

/**
 * @brief Print a list of all active processes to the terminal.
 */
void task_list() {
    char buf[12];
    uint32_t flags;
    spin_lock_irqsave(&tasklist_lock, &flags);

    terminal_writestring("PID  STATE     EIP         NAME\n");
    for (int i = 0; i < cpu_count; i++) {
    task_t* t = (task_t*)cpus[i].ready_queue;
    while (t) {
        /* PID */
        int_to_string(t->id, buf);
        terminal_writestring(buf);
        terminal_putchar(' ');

        /* State */
        if (t->state == STATE_RUNNING) terminal_writestring("RUNNING ");
        else if (t->state == STATE_READY) terminal_writestring("READY   ");
        else if (t->state == STATE_SLEEPING) terminal_writestring("SLEEP   ");
        else if (t->state == STATE_ZOMBIE) terminal_writestring("ZOMBIE  ");
        else terminal_writestring("UNKNOWN ");

        /* EIP */
        format_hex(t->eip, buf);
        terminal_writestring(buf);
        terminal_putchar(' ');

        /* Name */
        terminal_writestring(t->name);
        terminal_writestring("\n");

        t = t->next;
    }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
}

/**
 * @brief Return the PID of the current task.
 */
int getpid() {
    return cur_task()->id;
}

/**
 * @brief Mark a task by PID as a zombie.
 * @param pid The PID to kill.
 * @return 0 on success, -1 if not found.
 */
device_init(init_tasking);

/**
 * @brief Register a signal handler for the current task.
 * @param sig Signal number (1-31).
 * @param handler SIG_IGN (1), SIG_DFL (0), or a function pointer.
 * @return Previous handler, or SIG_ERR on invalid sig.
 */
void* sys_signal(int sig, void* handler) {
    if (sig < 1 || sig > 31 || sig == SIGKILL) {
        return SIG_ERR;
    }
    void* prev = cur_task()->signal_handlers[sig];
    cur_task()->signal_handlers[sig] = handler;
    return prev;
}

/**
 * @brief Send a signal to a process.
 * @param pid Target process ID.
 * @param sig Signal number (1-31).
 * @return 0 on success, -1 if PID not found or sig invalid.
 */
int sys_kill(int pid, int sig) {
    if (sig < 1 || sig > 31) return -EINVAL;

    /* Never kill PID 1 (the shell/init) */
    if (pid == 1) return -EPERM;

    uint32_t flags;
    spin_lock_irqsave(&tasklist_lock, &flags);

    /* Scan ALL CPU ready queues for the target PID */
    for (int i = 0; i < cpu_count; i++) {
        task_t* t = (task_t*)cpus[i].ready_queue;
        while (t) {
            if (t->id == pid) {
                /* Atomic: the keyboard ISR sets SIGINT on this same word */
                __sync_fetch_and_or(&t->signal_pending, 1u << sig);
                spin_unlock_irqrestore(&tasklist_lock, flags);
                return 0;
            }
            t = t->next;
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return -ESRCH;
}

int task_kill(int pid) {
    return sys_kill(pid, SIGTERM);
}

/* Set by the shell around waitpid() for a child, cleared afterwards.
 * Read by the keyboard ISR to route Ctrl+C. */
volatile int fg_child_pid = 0;

task_t* task_from_pid(int pid) {
    uint32_t flags;
    spin_lock_irqsave(&tasklist_lock, &flags);
    for (int i = 0; i < cpu_count; i++) {
        task_t* t = (task_t*)cpus[i].ready_queue;
        while (t) {
            if (t->id == pid) {
                spin_unlock_irqrestore(&tasklist_lock, flags);
                return t;
            }
            t = t->next;
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return NULL;
}

/**
 * @brief Give an AP an idle task so its scheduler has something to run
 *        from the first LAPIC tick.
 *
 * Reuses the AP's boot stack as the idle kernel stack — the AP is
 * executing on it right now, and its top was already programmed as the
 * AP's ESP0 source.  Called by smp_init() on the BSP before the AP is
 * woken.
 *
 * @param cpu_index Logical CPU number (>= 1).
 */
void task_spawn_idle(int cpu_index)
{
    cpu_t* c = &cpus[cpu_index];
    if (!c->kstack_top || !c->page_directory) return;

    task_t* idle = kmalloc(sizeof(task_t));
    if (!idle) {
        pr_err("no memory for idle task on CPU %d\n", cpu_index);
        return;
    }

    memset(idle, 0, sizeof(*idle));

    uint32_t flags;
    spin_lock_irqsave(&pid_lock, &flags);
    idle->id = next_pid++;   /* must land after the memset above */
    spin_unlock_irqrestore(&pid_lock, flags);

    idle->page_directory = (page_directory_t*)c->page_directory;
    idle->kstack = c->kstack_top - KSTACK_SIZE;
    idle->eip = (uint32_t)idle_loop;
    idle->state = STATE_READY;
    strcpy(idle->name, "idle");

    spin_lock_irqsave(&tasklist_lock, &flags);
    c->ready_queue   = (struct task*)idle;
    c->current_task  = (struct task*)idle;
    spin_unlock_irqrestore(&tasklist_lock, flags);
}
