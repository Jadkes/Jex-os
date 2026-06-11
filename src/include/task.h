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
    char name[16];                      /**< Human-readable task name */
    uint32_t cpu_ticks;                 /**< CPU ticks consumed (for top command). */
    struct task* next;                  /**< Next task in the linked scheduler list. */

    /* Signal handling */
    uint32_t signal_pending;            /**< Bitmask: pending signals (set by kill, cleared by delivery) */
    uint32_t signal_blocked;            /**< Bitmask: blocked signals */
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

/**
 * @brief Mark a task by PID as a zombie.
 * @param pid The PID to kill.
 * @return 0 on success, -1 if not found.
 */
int task_kill(int pid);

/* Signal syscalls */
void* sys_signal(int sig, void* handler);
int   sys_kill(int pid, int sig);

/**
 * @brief Global current-task and ready-queue pointers (defined in task.c).
 */
extern volatile task_t* current_task;
extern volatile task_t* ready_queue;

#endif // TASK_H
