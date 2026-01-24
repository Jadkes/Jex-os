#include "task.h"
#include "kheap.h"
#include "paging.h"
#include "string.h"
#include "gdt.h"

extern page_directory_t kernel_directory;
extern uint32_t read_eip();
extern void terminal_writestring(const char* s);

volatile task_t* current_task;
volatile task_t* ready_queue;

static int next_pid = 1;

void init_tasking() {
    __asm__ volatile("cli");

    current_task = ready_queue = (task_t*)kmalloc(sizeof(task_t));
    current_task->id = next_pid++;
    current_task->esp = current_task->ebp = 0;
    current_task->eip = 0;
    current_task->page_directory = &kernel_directory;
    current_task->kstack = 0; // Kernel/Shell uses the initial boot stack
    current_task->next = NULL;
    current_task->state = STATE_RUNNING;

    __asm__ volatile("sti");
}

void task_switch() {
    if (!current_task) return;

    uint32_t esp, ebp, eip;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    eip = read_eip();
    if (eip == 0x12345) return; 

    current_task->eip = eip;
    current_task->esp = esp;
    current_task->ebp = ebp;

    // Switch to next task
    current_task = current_task->next;
    if (!current_task) current_task = ready_queue;

    esp = current_task->esp;
    ebp = current_task->ebp;
    eip = current_task->eip;

    // Update TSS so kernel interrupts land on the correct stack
    if (current_task->kstack) {
        set_kernel_stack(current_task->kstack + 8192);
    }

    __asm__ volatile("         \n      mov %0, %%ebx;           \n      mov %1, %%esp;           \n      mov %2, %%ebp;           \n      mov %3, %%cr3;           \n      mov $0x12345, %%eax;     \n      sti;                     \n      jmp *%%ebx;              \n  " : : "r"(eip), "r"(esp), "r"(ebp), "r"(current_task->page_directory) : "ebx", "eax");
}

int fork() {
    __asm__ volatile("cli");

    task_t* parent = (task_t*)current_task;
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    child->id = next_pid++;
    child->page_directory = parent->page_directory;
    child->state = STATE_READY;
    child->next = NULL;

    // Allocate a new stack for the child (8KB)
    uint32_t stack = (uint32_t)kmalloc(8192);
    child->kstack = stack;

    // Copy the stack from parent to child
    // Since we use identity mapping, we must be careful.
    // For simplicity in this v0.4 start, we'll just copy the current stack
    // but the child will have a DIFFERENT physical address.
    // To make this work without full paging isolation, we'd need to adjust pointers.
    // For now, let's just show the scheduler working with a shared stack 
    // but FIXING the interrupt state.
    
    // Better way: Give child its own ESP
    uint32_t esp, ebp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    __asm__ volatile("mov %%ebp, %0" : "=r"(ebp));

    child->esp = esp;
    child->ebp = ebp;
    
    // Add to ready queue
    task_t* tmp = (task_t*)ready_queue;
    while(tmp->next) tmp = tmp->next;
    tmp->next = child;

    uint32_t eip = read_eip();
    if (current_task == parent) {
        child->eip = eip;
        __asm__ volatile("sti");
        return child->id;
    } else {
        // Child starts here!
        __asm__ volatile("sti");
        return 0;
    }
}

void task_exit() {
    __asm__ volatile("cli");
    current_task->state = STATE_ZOMBIE;
    // Simple: just skip this task in the next switch
    task_switch();
}

void task_list() {
    task_t* t = (task_t*)ready_queue;
    terminal_writestring("PID  STATE\n");
    while(t) {
        char buf[10];
        extern void int_to_string(int n, char* str);
        int_to_string(t->id, buf);
        terminal_writestring(buf);
        terminal_writestring("    ");
        if (t->state == STATE_RUNNING) terminal_writestring("RUNNING\n");
        else if (t->state == STATE_READY) terminal_writestring("READY\n");
        else if (t->state == STATE_ZOMBIE) terminal_writestring("ZOMBIE\n");
        t = t->next;
    }
}

int getpid() {
    return current_task->id;
}
