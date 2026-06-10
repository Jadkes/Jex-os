/**
 * @file device.c
 * @brief Generic device registration implementation.
 *
 * Maintains a simple singly-linked list of all registered devices.
 * Upper layers use iter_first/iter_next to enumerate discovered hardware.
 *
 * Thread-safety: Called once during boot, no concurrency.
 */

#include "device.h"
#include <stddef.h>

static struct device* device_list = NULL;

void device_register(struct device* dev)
{
    dev->next = device_list;
    device_list = dev;
}

struct device* device_iter_first(void)
{
    return device_list;
}

struct device* device_iter_next(struct device* prev)
{
    return prev ? prev->next : NULL;
}
