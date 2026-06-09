/**
 * @file task.c
 * @brief Preemptive multitasking and process management.
 *
 * Implements a simple round-robin scheduler and task switching logic.
 */

#include "task.h"
#include "kheap.h"
#include "paging.h"
#include "string.h"
#include "gdt.h"
#include "panic.h"

extern page_directory_t kernel_directory;
extern uint32_t read_eip();
extern void terminal_writestring(const char* s);
extern void terminal_putchar(char c);
extern void int_to_string(int n, char* str);

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
    current_task->kstack = 0; /* Shell uses the initial boot stack */
    current_task->next = NULL;
    current_task->state = STATE_RUNNING;
    strcpy((char*)current_task->name, "shell");

    __asm__ volatile("sti");
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

    /* Round-robin: Pick the next task in the circular list */
    current_task = current_task->next;
    if (!current_task) current_task = ready_queue;

    esp = current_task->esp;
    ebp = current_task->ebp;
    eip = current_task->eip;

    /* Update TSS so hardware interrupts land on the correct kernel stack */
    if (current_task->kstack) {
        set_kernel_stack(current_task->kstack + 8192);
    }

    /**
     * @brief Assembly context switch.
     * Loads the new stack pointers and jumps to the new instruction pointer.
     */
    __asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "r"(eip), "r"(esp), "r"(ebp), "r"(current_task->page_directory) : "ebx", "eax");
}

/**
 * @brief Create a new process (child) by duplicating the current one (parent).
 * @return 0 for the child process, PID of child for the parent process.
 */
int fork() {
    __asm__ volatile("cli");

    task_t* parent = (task_t*)current_task;
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    child->id = next_pid++;
    child->page_directory = parent->page_directory;
    child->state = STATE_READY;
    child->next = NULL;
    strcpy(child->name, parent->name);

    /* Allocate a dedicated kernel stack for the child */
    uint32_t stack = (uint32_t)kmalloc(8192);
    child->kstack = stack;

    uint32_t esp, ebp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    child->esp = esp;
    child->ebp = ebp;
    
    /* Add the child to the end of the ready queue */
    task_t* tmp = (task_t*)ready_queue;
    while(tmp->next) tmp = tmp->next;
    tmp->next = child;

    uint32_t eip = read_eip();
    if (current_task == parent) {
        child->eip = eip;
        __asm__ volatile("sti");
        return child->id;
    } else {
        /* This is the child process starting for the first time! */
        __asm__ volatile("sti");
        return 0;
    }
}

/**
 * @brief Mark the current task as finished and switch to the next one.
 */
void task_exit() {
    __asm__ volatile("cli");
    current_task->state = STATE_ZOMBIE;
    task_switch();
}

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
int task_kill(int pid)
{
    /* Kernel-level guard: never kill the init/shell task */
    if (pid == 1)
        return -1;
    task_t* t = (task_t*)ready_queue;
    while (t) {
        if (t->id == pid) {
            t->state = STATE_ZOMBIE;
            return 0;
        }
        t = t->next;
    }
    return -1;
}
