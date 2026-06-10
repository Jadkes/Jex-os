#ifndef KALLSYMS_H
#define KALLSYMS_H
#include <stdint.h>

typedef struct {
    uint32_t addr;
    uint32_t name_off : 24;
    uint32_t size      : 8;
} __attribute__((packed)) kallsym_entry_t;

void kallsyms_init(void);
const char* kallsyms_lookup(uint32_t addr, uint32_t* offset, uint32_t* size);

#endif /* KALLSYMS_H */
