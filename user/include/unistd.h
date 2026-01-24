#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// File descriptor constants
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

// POSIX file operations
int open(const char* pathname, int flags);
int close(int fd);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);

// Memory management
void* sbrk(intptr_t increment);

// Process management
int fork(void);
int execve(const char* filename, char* const argv[], char* const envp[]);
void _exit(int status);

// Misc
int getpid(void);
int isatty(int fd);

#endif