/**
 * @file workqueue.c
 * @brief Deferred workqueue for ISR-safe execution.
 *
 * Design:
 * - schedule_work() is safe from ISR context using cli/sti save/restore.
 * - workqueue_run() executes pending items from the shell main loop.
 * - Single, global linked list with head and tail pointers for O(1) append.
 */

#include <stddef.h>
#include <stdint.h>
#include "kernel/workqueue.h"

static struct work* work_head = NULL;
static struct work* work_tail = NULL;

void schedule_work(struct work* w)
{
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile("cli");

    w->next = NULL;
    if (work_tail)
        work_tail->next = w;
    else
        work_head = w;
    work_tail = w;

    if (eflags & 0x200)
        __asm__ volatile("sti");
}

void workqueue_run(void)
{
    while (1) {
        struct work* w;

        __asm__ volatile("cli");
        w = work_head;
        if (w) {
            work_head = w->next;
            if (!work_head)
                work_tail = NULL;
        }
        __asm__ volatile("sti");

        if (!w) break;
        w->func(w->data);
    }
}
