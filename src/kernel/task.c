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
#include "kernel/backtrace.h"

extern page_directory_t kernel_directory;
extern uint32_t read_eip();
extern void terminal_writestring(const char* s);
extern void terminal_putchar(char c);
extern void int_to_string(int n, char* str);

/* Declared in kernel_fork_trampoline asm label below */
extern void kernel_fork_trampoline(void);

/**
 * @brief Global pointers to the current task and the head of the ready queue.
 */
volatile task_t* current_task;
volatile task_t* ready_queue;

static int next_pid = 1;

/**
 * @brief Initialize multitasking.
 * Creates the first task (Kernel/Shell) and starts the scheduler.
 */
void init_tasking() {
    __asm__ volatile("cli");

    current_task = ready_queue = (task_t*)kmalloc(sizeof(task_t));
    current_task->id = next_pid++;
    current_task->esp = current_task->ebp = 0;
    current_task->eip = 0;
    current_task->page_directory = &kernel_directory;
    current_task->kstack = alloc_kernel_stack(current_task->id);
    current_task->cpu_ticks = 0;
    current_task->next = NULL;
    current_task->state = STATE_RUNNING;
    current_task->parent_pid = 0;
    current_task->exit_code = 0;
    current_task->signal_pending = 0;
    current_task->signal_blocked = 0;
    for (int i = 0; i < 32; i++)
        current_task->signal_handlers[i] = NULL;
    strcpy((char*)current_task->name, "shell");

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
    if (!current_task) return;

    uint32_t esp, ebp, eip;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    /* Magic return for the next time we are switched TO */
    eip = read_eip();
    if (eip == 0x12345) return;

    current_task->eip = eip;
    current_task->esp = esp;
    current_task->ebp = ebp;

    /* Round-robin: Pick the next task in the ready queue */
    volatile task_t* start = current_task;
    current_task = current_task->next;
    if (!current_task) current_task = ready_queue;

    /* Skip zombie tasks — they stay around for the parent to reap */
    {
        while (current_task->state == STATE_ZOMBIE) {
            current_task = current_task->next;
            if (!current_task) current_task = ready_queue;
            /* If we wrapped around and the ONLY non-zombie task is start
             * itself, the while-loop check re-evaluates start's state on the
             * next iteration.  But if start is ALSO zombie, every task is
             * zombie and there's nothing to schedule. */
            if (current_task == start && current_task->state == STATE_ZOMBIE) {
                /* All tasks are zombies — nothing to run */
                for (;;) asm volatile("hlt");
            }
        }
    }

    /* Deliver any pending signals before the task runs */
    while (deliver_signals((task_t*)current_task)) {
        task_t* terminated = (task_t*)current_task;
        task_t* prev = (task_t*)ready_queue;
        if (prev == terminated) {
            ready_queue = terminated->next;
        } else {
            while (prev && prev->next != terminated)
                prev = prev->next;
            if (prev)
                prev->next = terminated->next;
        }
        current_task = terminated->next;
        if (!current_task) current_task = ready_queue;
        if (!current_task)
            panic_assert("All tasks terminated by signals", __FILE__, __LINE__);
    }

    esp = current_task->esp;
    ebp = current_task->ebp;
    eip = current_task->eip;

    if (current_task->kstack) {
        set_kernel_stack(current_task->kstack + 8192);
    }


    __asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "b"(eip), "r"(esp), "r"(ebp), "r"(current_task->page_directory) : "eax");
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
    __asm__ volatile("cli");

    task_t* parent = (task_t*)current_task;

    /* Allocate task_t via kmalloc so waitpid's kfree() matches.
     * Previously used pmm_alloc_block() which kfree() cannot free
     * (ptr_to_slab sees task_t.id as "magic" — never matches). */
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) {
        __asm__ volatile("sti");
        return -1;
    }

    /* Clone the page directory */
    page_directory_t* new_dir = clone_page_directory(parent->page_directory);
    if (!new_dir) {
        kfree(child);
        __asm__ volatile("sti");
        return -1;
    }

    child->id = next_pid++;
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
    child->kstack = alloc_kernel_stack(child->id);
    if (!child->kstack) {
        __asm__ volatile("sti");
        return -1;
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


    /* Add child to end of ready queue */
    task_t* tmp = (task_t*)ready_queue;
    while (tmp->next) tmp = tmp->next;
    tmp->next = child;

    __asm__ volatile("sti");
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
int kernel_fork(void (*func)(void*), void* arg)
{
    __asm__ volatile("cli");

    task_t* parent = (task_t*)current_task;
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) {
        __asm__ volatile("sti");
        return -1;
    }

    /* Clone the page directory — deep-copy user pages, share kernel pages */
    page_directory_t* new_dir = clone_page_directory(parent->page_directory);
    if (!new_dir) {
        kfree(child);
        __asm__ volatile("sti");
        return -1;
    }

    child->id = next_pid++;
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
    child->kstack = alloc_kernel_stack(child->id);
    if (!child->kstack) {
        kfree(child);
        __asm__ volatile("sti");
        return -1;
    }

    /*
     * Set up the child's initial stack for the trampoline.
     * Push arg first, then func on top — the trampoline pops func first (ebx),
     * then arg (eax), then pushes arg back and calls *ebx (func(arg)).
     */
    uint32_t* cp = (uint32_t*)(child->kstack + KSTACK_SIZE);
    *--cp = (uint32_t)arg;   /* arg — popped second */
    *--cp = (uint32_t)func;  /* func — popped first */

    child->esp = (uint32_t)cp;
    child->ebp = (uint32_t)cp;
    child->eip = (uint32_t)kernel_fork_trampoline;

    /* Add child to end of ready queue */
    task_t* tmp = (task_t*)ready_queue;
    while (tmp->next) tmp = tmp->next;
    tmp->next = child;

    __asm__ volatile("sti");
    return child->id;
}

/**
 * @brief Exit the current task with an exit code.
 *
 * Sets state to ZOMBIE but stays in the ready queue so the parent
 * can find and reap it via waitpid(). The scheduler skips zombies.
 */
void _task_exit(int exit_code) {
    __asm__ volatile("cli");

    task_t* dying = (task_t*)current_task;

    /* PID 1 (shell/init) cannot be killed this way */
    if (dying->id == 1) {
        __asm__ volatile("sti");
        return;
    }

    {
        extern void log_serial(const char* str);
        extern void log_hex_serial(uint32_t n);
        log_serial("[EXIT] pid=");
        log_hex_serial(dying->id);
        log_serial(" code=");
        log_hex_serial(exit_code);
        log_serial("\n");
    }

    /* Reparent orphans: children of the dying task become children of init (PID 1) */
    {
        for (task_t* scan = (task_t*)ready_queue; scan; scan = scan->next) {
            if (scan->parent_pid == dying->id) {
                scan->parent_pid = 1;
                extern void log_serial(const char* str);
                extern void log_hex_serial(uint32_t n);
                log_serial("[REPARENT] pid=");
                log_hex_serial(scan->id);
                log_serial(" to init\n");
            }
        }
    }

    dying->exit_code = exit_code;
    dying->state = STATE_ZOMBIE;

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
int waitpid(int pid, int* status, int options) {
    (void)options;

    task_t* self = (task_t*)current_task;

    while (1) {
        /* Scan the ready queue for a zombie child */
        task_t* prev = NULL;
        for (task_t* scan = (task_t*)ready_queue; scan; scan = scan->next) {
            if (scan->state == STATE_ZOMBIE &&
                scan->parent_pid == self->id &&
                (pid == -1 || scan->id == pid)) {
                /* Found — reap it */
                if (status)
                    *status = scan->exit_code;
                int child_pid = scan->id;
                {
                    extern void log_serial(const char* str);
                    extern void log_hex_serial(uint32_t n);
                    log_serial("[WAITPID] reaping pid=");
                    log_hex_serial(child_pid);
                    log_serial(" by pid=");
                    log_hex_serial(self->id);
                    log_serial(" code=");
                    log_hex_serial(scan->exit_code);
                    log_serial("\n");
                }

                /* Free kernel stack — PDE 1023 shared page table */
                if (scan->kstack)
                    free_kernel_stack(scan->id);

                /* TODO: Free the child's page directory and all cloned
                 * page tables/pages.  Currently leaked — each fork() or
                 * kernel_fork() that exits leaks ~10+ physical pages.
                 * This is a known limitation; a full page-directory teardown
                 * function is needed (walk all user PDEs, free page tables
                 * and user pages, then free the directory frame itself). */

                /* Remove from ready queue */
                if (prev)
                    prev->next = scan->next;
                else
                    ready_queue = scan->next;

                kfree(scan);

                /* After reaping the requested child, also reap any orphaned
                 * zombies that were reparented to us (PID 1 / init).  This
                 * happens when a grandchild outlives its parent — the parent
                 * exits, the grandchild is reparented to init, and if the
                 * grandchild is already a zombie, nobody else will reap it.
                 * Without this, orphan zombies leak their kernel stack pages
                 * in the shared PDE 1023 page table. */
                {
                    task_t* p = NULL;
                    task_t* s = (task_t*)ready_queue;
                    while (s) {
                        if (s->state == STATE_ZOMBIE &&
                            s->parent_pid == self->id && s->id != self->id) {
                            task_t* dead = s;
                            if (p)
                                p->next = dead->next;
                            else
                                ready_queue = dead->next;
                            s = dead->next;
                            if (dead->kstack)
                                free_kernel_stack(dead->id);
                            kfree(dead);
                        } else {
                            p = s;
                            s = s->next;
                        }
                    }
                }

                __asm__ volatile("sti");  /* re-enable interrupts before returning */
                return child_pid;
            }
            prev = scan;
        }

        /* Check if any children exist at all */
        {
            int any = 0;
            for (task_t* scan = (task_t*)ready_queue; scan; scan = scan->next) {
                if (scan->parent_pid == self->id) {
                    any = 1;
                    break;
                }
            }
            if (!any)
                return -1;  /* No children — ECHILD */
        }

        /* Yield to other tasks while waiting */
        __asm__ volatile("sti");
        __asm__ volatile("pause");
        __asm__ volatile("cli");
    }
}

void _waitpid_done(void) {} /* marker for breakpoint */

/**
 * @brief Print a list of all active processes to the terminal.
 */
void task_list() {
    task_t* t = (task_t*)ready_queue;
    char buf[12];

    terminal_writestring("PID  STATE     EIP         NAME\n");
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

/**
 * @brief Return the PID of the current task.
 */
int getpid() {
    return current_task->id;
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
    void* prev = current_task->signal_handlers[sig];
    current_task->signal_handlers[sig] = handler;
    return prev;
}

/**
 * @brief Send a signal to a process.
 * @param pid Target process ID.
 * @param sig Signal number (1-31).
 * @return 0 on success, -1 if PID not found or sig invalid.
 */
int sys_kill(int pid, int sig) {
    if (sig < 1 || sig > 31) return -1;

    /* Never kill PID 1 (the shell/init) */
    if (pid == 1) return -1;

    task_t* t = (task_t*)ready_queue;
    while (t) {
        if (t->id == pid) {
            t->signal_pending |= (1 << sig);
            return 0;
        }
        t = t->next;
    }
    return -1;
}

int task_kill(int pid) {
    return sys_kill(pid, SIGTERM);
}
