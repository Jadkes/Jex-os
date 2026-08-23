/*
 * util.h - Common kernel macros
 *
 * Purpose: The small glue macros every subsystem ends up wanting --
 *          clamping, container_of for intrusive structures, array
 *          sizing.  Kept dependency-free so any header can pull it in.
 */

#ifndef UTIL_H
#define UTIL_H

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Bound x to [lo, hi]; arguments are evaluated once each */
#define CLAMP(x, lo, hi) \
    ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/* Elements in a real C array; nonsense on pointers -- that is the point */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/**
 * container_of - Recover the enclosing structure from a member pointer
 * @ptr:    pointer to the member field
 * @type:   enclosing struct type
 * @member: name of the member inside @type
 *
 * The standard intrusive-data-structure trick: given a &task->queue
 * node you can walk back to its owning task without storing a
 * back-pointer.
 */
#define container_of(ptr, type, member)                      \
    ((type*)((char*)(ptr) - offsetof(type, member)))

/* Align up/down to a power-of-two boundary */
#define ALIGN_DOWN(x, a) ((x) & ~((a)-1))
#define ALIGN_UP(x, a)   ALIGN_DOWN((x) + (a)-1, (a))

#endif /* UTIL_H */
