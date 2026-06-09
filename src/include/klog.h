#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

#define KLOG_SIZE 4096    /* 4 KB ring buffer */
#define KLOG_LEVEL_ERROR 0
#define KLOG_LEVEL_WARN  1
#define KLOG_LEVEL_INFO  2
#define KLOG_LEVEL_DEBUG 3

void klog_init(void);
void klog_write(int level, const char* str);
void klog_dump(void);
void klog_set_level(int level);
int  klog_get_level(void);

#endif /* KLOG_H */
