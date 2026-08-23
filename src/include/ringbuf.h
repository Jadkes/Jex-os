/*
 * ringbuf.h - Generic single-producer single-consumer byte ring
 *
 * Purpose: One reviewed implementation for the kernel's byte queues --
 *          keyboard input, serial RX, anything where an ISR produces
 *          and a task consumes.  The SPSC layout means no locks: the
 *          producer owns head, the consumer owns tail, and indices are
 *          volatile so neither side caches a stale value.
 *
 * Thread-safety: Exactly one producer context and one consumer
 *          context.  Multiple producers or consumers need an external
 *          lock.  The buffer memory is caller-provided (no allocation
 *          here), sized to a power of two so masking replaces the mod.
 */

#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    volatile uint8_t* data;
    uint16_t mask;        /* size - 1; requires power-of-two capacity */
    volatile uint16_t head;  /* producer-owned: next write slot */
    volatile uint16_t tail;  /* consumer-owned: next read slot */
} ringbuf_t;

/**
 * ringbuf_init - Attach a ring to caller-provided storage
 * @rb:   ring to initialise
 * @storage: buffer of @size bytes (power of two)
 * @size: capacity in bytes
 */
static inline void ringbuf_init(ringbuf_t* rb,
                                volatile uint8_t* storage,
                                uint16_t size)
{
    rb->data = storage;
    rb->mask = size - 1;
    rb->head = 0;
    rb->tail = 0;
}

/**
 * ringbuf_put - Push one byte from producer context
 * @rb: ring
 * @byte: value to enqueue
 * @return 0 on success, -1 if full (byte dropped by caller choice)
 */
static inline int ringbuf_put(ringbuf_t* rb, uint8_t byte)
{
    uint16_t next = (uint16_t)((rb->head + 1) & rb->mask);
    if (next == rb->tail)
        return -1;   /* full -- one slot kept empty to tell full from empty */
    rb->data[rb->head] = byte;
    rb->head = next;
    return 0;
}

/**
 * ringbuf_get - Pop one byte from consumer context
 * @rb: ring
 * @return the byte, or -1 when empty
 */
static inline int ringbuf_get(ringbuf_t* rb)
{
    if (rb->head == rb->tail)
        return -1;
    uint8_t byte = rb->data[rb->tail];
    rb->tail = (uint16_t)((rb->tail + 1) & rb->mask);
    return byte;
}

/**
 * ringbuf_has_data - True when at least one byte is pending
 * @rb: ring
 */
static inline int ringbuf_has_data(const ringbuf_t* rb)
{
    return rb->head != rb->tail;
}

/**
 * ringbuf_reset - Drop all pending bytes
 * @rb: ring
 *
 * Only safe when producer and consumer are quiescent (e.g. with IRQs
 * masked) since both indices move.
 */
static inline void ringbuf_reset(ringbuf_t* rb)
{
    rb->head = 0;
    rb->tail = 0;
}

#endif /* RINGBUF_H */
