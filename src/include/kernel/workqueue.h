#ifndef WORKQUEUE_H
#define WORKQUEUE_H

typedef void (*work_func_t)(void* data);

struct work {
    work_func_t  func;
    void*        data;
    struct work* next;
};

void schedule_work(struct work* w);
void workqueue_run(void);

#endif /* WORKQUEUE_H */
