#ifndef JEXOS_ERRNO_H
#define JEXOS_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Errno codes (POSIX-compatible values) ── */

#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3   /* No such process */
#define EINTR    4   /* Interrupted system call */
#define EIO      5   /* I/O error */
#define ENXIO    6   /* No such device or address */
#define E2BIG    7   /* Argument list too long */
#define ENOEXEC  8   /* Exec format error */
#define EBADF    9   /* Bad file number */
#define ECHILD  10   /* No child processes */
#define EAGAIN  11   /* Try again */
#define ENOMEM  12   /* Out of memory */
#define EACCES  13   /* Permission denied */
#define EFAULT  14   /* Bad address */
#define EBUSY   16   /* Device or resource busy */
#define EEXIST  17   /* File exists */
#define ENODEV  19   /* No such device */
#define ENOTDIR 20   /* Not a directory */
#define EISDIR  21   /* Is a directory */
#define EINVAL  22   /* Invalid argument */
#define EFBIG   27   /* File too large */
#define ENOSPC  28   /* No space left on device */
#define ESPIPE  29   /* Illegal seek */
#define ERANGE        34   /* Result too large */
#define ENAMETOOLONG  36   /* File name too long */
#define ENOSYS        38   /* Function not implemented */

/* ── IS_ERR / PTR_ERR helpers ── */

#define IS_ERR(val)       ((int)(val) < 0)
#define ERR_VAL(val)      ((int)(val))
#define ERR_PTR(err)      ((void*)(long)(-(err)))
#define PTR_ERR(ptr)      ((int)(long)(ptr))
#define IS_ERR_OR_NULL(p) (!(p) || ((int)(long)(p) < 0))

#ifdef __cplusplus
}
#endif

#endif /* JEXOS_ERRNO_H */
