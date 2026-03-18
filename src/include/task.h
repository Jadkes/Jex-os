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

/**
 * @enum task_state_t
 * @brief Current execution state of a task.
 */
typedef enum {
    STATE_READY,        /**< Task is waiting to be scheduled. */
    STATE_RUNNING,      /**< Task is currently executing. */
    STATE_SLEEPING,     /**< Task is waiting for a timer or event. */
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
    task_state_t state;                 /**< Current execution state. */
    struct task* next;                  /**< Next task in the circular scheduler list. */
} task_t;

/**
 * @brief Initialize the multitasking system.
 */
void init_tasking();

/**
 * @brief Force a context switch to the next ready task.
 */
void task_switch();

/**
 * @brief Create a new process by duplicating the current one.
 * @return 0 in the child, child PID in the parent.
 */
int fork();

/**
 * @brief Get the PID of the currently running task.
 */
int getpid();

/**
 * @brief Exit the current task and mark it as a zombie.
 */
void task_exit();

/**
 * @brief List all active tasks to the terminal (for debugging).
 */
void task_list();

#endif // TASK_H
