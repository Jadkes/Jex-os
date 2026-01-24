#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "paging.h"

// State of a process
typedef enum {
    STATE_READY,
    STATE_RUNNING,
    STATE_SLEEPING,
    STATE_ZOMBIE
} task_state_t;

// Process Control Block (PCB)
typedef struct task {
    int id;                // Process ID
    uint32_t esp, ebp;     // Stack and base pointers
    uint32_t eip;          // Instruction pointer
    page_directory_t* page_directory; // Process's page directory
    uint32_t kstack;       // Kernel stack base
    task_state_t state;    // Process state
    struct task* next;     // Next task in linked list
} task_t;

void init_tasking();
void task_switch();
int fork();
int getpid();
void task_exit();
void task_list();

#endif