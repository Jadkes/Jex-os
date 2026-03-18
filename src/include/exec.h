/**
 * @file exec.h
 * @brief Program execution and script loading interface.
 */

#ifndef EXEC_H
#define EXEC_H

#include <stdint.h>

/**
 * @brief Execute a file with arguments and environment.
 * 
 * Supports both ELF binaries and shebang scripts.
 * @param filename Path to the executable.
 * @param argv NULL-terminated list of arguments.
 * @param envp NULL-terminated list of environment variables.
 * @return 0 on success, -1 on failure.
 */
int execve_file(const char* filename, char** argv, char** envp);

/**
 * @brief Check if a file starts with a shebang line (#!).
 * 
 * @param filename Path to the file.
 * @param interpreter Output buffer for the interpreter path.
 * @param interp_size Size of the output buffer.
 * @return 0 if found, -1 otherwise.
 */
int check_shebang(const char* filename, char* interpreter, int interp_size);

/**
 * @brief Compile and run C source code directly.
 * 
 * @param c_source Null-terminated string of C code.
 * @param argv Arguments to pass to the compiled program.
 * @return 0 on success, -1 on failure.
 */
int exec_c_code(const char* c_source, char** argv);

#endif // EXEC_H
