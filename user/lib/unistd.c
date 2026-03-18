/**
 * @file unistd.c
 * @brief Standard POSIX-like system calls for user-space.
 */

#include "../include/unistd.h"
#include "../include/jexos.h"

/* Simplified file open flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

/**
 * @brief Open a file.
 */
int open(const char* pathname, int flags) {
    return sys_open(pathname, flags);
}

/**
 * @brief Close a file.
 */
int close(int fd) {
    return sys_close(fd);
}

/**
 * @brief Read from a file.
 */
ssize_t read(int fd, void* buf, size_t count) {
    return sys_read(fd, buf, count);
}

/**
 * @brief Write to a file.
 */
ssize_t write(int fd, const void* buf, size_t count) {
    return sys_write(fd, buf, count);
}

/**
 * @brief Move the read/write offset.
 */
off_t lseek(int fd, off_t offset, int whence) {
    return sys_seek(fd, offset, whence);
}

/**
 * @brief Request heap expansion.
 */
void* sbrk(intptr_t increment) {
    return sys_sbrk(increment);
}

/**
 * @brief Create a child process (stub).
 */
int fork(void) {
    return -1; 
}

/**
 * @brief Execute a file (stub).
 */
int execve(const char* filename, char* const argv[], char* const envp[]) {
    (void)filename; (void)argv; (void)envp;
    return -1; 
}

/**
 * @brief Exit the process with status code.
 */
void _exit(int status) {
    (void)status;
    sys_exit();
}

/**
 * @brief Return current process ID.
 */
int getpid(void) {
    return 1; 
}

/**
 * @brief Check if a file descriptor is a terminal.
 */
int isatty(int fd) {
    return (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) ? 1 : 0;
}
