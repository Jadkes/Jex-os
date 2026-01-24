#include "../include/unistd.h"
#include "../include/jexos.h"

// File open flags (simplified for now)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

int open(const char* pathname, int flags) {
    return sys_open(pathname, flags);
}

int close(int fd) {
    return sys_close(fd);
}

ssize_t read(int fd, void* buf, size_t count) {
    return sys_read(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return sys_write(fd, buf, count);
}

off_t lseek(int fd, off_t offset, int whence) {
    return sys_seek(fd, offset, whence);
}

void* sbrk(intptr_t increment) {
    return sys_sbrk(increment);
}

// Stub implementations for unsupported functions
int fork(void) {
    return -1; // Not implemented
}

int execve(const char* filename, char* const argv[], char* const envp[]) {
    (void)filename; (void)argv; (void)envp;
    return -1; // Not implemented
}

void _exit(int status) {
    (void)status;
    sys_exit();
}

int getpid(void) {
    return 1; // Single process for now
}

int isatty(int fd) {
    return (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) ? 1 : 0;
}