/*
 * sysfs.c - Kernel state exported as virtual files under /sys/
 *
 * Purpose: Exposes kernel-inspection and tuning knobs as virtual files
 *          mounted at /sys/.  Each file is backed by a pair of callbacks:
 *          .read() to produce content on-demand, and optionally .write()
 *          to accept input (e.g. changing log level).
 *
 * Design:
 *   - Each file is registered via devtmpfs_add_file() from sysfs_init().
 *   - sysfs_init() runs as a device_initcall so the VFS and devtmpfs
 *     are already set up when it fires.
 *
 * Files provided:
 *   /sys/kernel/version  - Kernel version string (read-only).
 *   /sys/kernel/loglevel - Console log level 0-7 (read-write).
 *   /sys/kernel/network  - Network subsystem status (read-only).
 *   /sys/kernel/meminfo  - Memory subsystem status (read-only).
 */

#include "devtmpfs.h"
#include "kernel/printk.h"
#include "init.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  /sys/kernel/version -- read only                                  */
/* ------------------------------------------------------------------ */

static int sys_read_version(char* buf, int max)
{
    const char* v = "JexOS v0.1 (i386)\n";
    int len = strlen(v);
    if (len > max)
        len = max;
    memcpy(buf, v, len);
    return len;
}

/* ------------------------------------------------------------------ */
/*  /sys/kernel/loglevel -- read-write                                */
/* ------------------------------------------------------------------ */

static int sys_read_loglevel(char* buf, int max)
{
    if (max < 2)
        return 0;
    buf[0] = '0' + (char)console_loglevel;
    buf[1] = '\n';
    return 2;
}

static int sys_write_loglevel(const char* buf, int len)
{
    if (len > 0 && buf[0] >= '0' && buf[0] <= '7')
        console_loglevel = buf[0] - '0';
    return len;
}

/* ------------------------------------------------------------------ */
/*  /sys/kernel/network -- read-only placeholder                      */
/* ------------------------------------------------------------------ */

static int sys_read_network(char* buf, int max)
{
    const char* info = "Network: active\n";
    int len = strlen(info);
    if (len > max)
        len = max;
    memcpy(buf, info, len);
    return len;
}

/* ------------------------------------------------------------------ */
/*  /sys/kernel/meminfo -- read-only placeholder                      */
/* ------------------------------------------------------------------ */

static int sys_read_meminfo(char* buf, int max)
{
    const char* info = "Memory: available\n";
    int len = strlen(info);
    if (len > max)
        len = max;
    memcpy(buf, info, len);
    return len;
}

/* ------------------------------------------------------------------ */
/*  Initcall -- register all sysfs files                              */
/* ------------------------------------------------------------------ */

void sysfs_init(void)
{
    devtmpfs_add_file("/sys/kernel/version", &(struct devtmpfs_file){
        .name = "version",
        .read = sys_read_version,
        .mode = 0444,
    });

    devtmpfs_add_file("/sys/kernel/loglevel", &(struct devtmpfs_file){
        .name = "loglevel",
        .read = sys_read_loglevel,
        .write = sys_write_loglevel,
        .mode = 0644,
    });

    devtmpfs_add_file("/sys/kernel/network", &(struct devtmpfs_file){
        .name = "network",
        .read = sys_read_network,
        .mode = 0444,
    });

    devtmpfs_add_file("/sys/kernel/meminfo", &(struct devtmpfs_file){
        .name = "meminfo",
        .read = sys_read_meminfo,
        .mode = 0444,
    });
}

device_init(sysfs_init);
