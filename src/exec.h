#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

// Execute a file with arguments and environment
int execve_file(const char* filename, char** argv, char** envp);

// Check if file starts with shebang
int check_shebang(const char* filename, char* interpreter, int interp_size);

// Execute C code using TCC
int exec_c_code(const char* c_source, char** argv);

#endif