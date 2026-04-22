/**
 * @file exec.c
 * @brief Program loader and execution support.
 *
 * This file is responsible for launching user-space programs (ELF binaries) 
 * and handling script execution via a shebang (#!) mechanism.
 */

#include "exec.h"
#include "fs.h"
#include "elf.h"
#include "tcc.h"
#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"
#include "syscall.h"
#include "terminal.h"
#include "serial.h"
#include "gdt.h"
#include <stddef.h>

/* Forward declarations for architecture-specific assembly functions */
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);

/**
 * @brief Inspect the first bytes of a file to check for a shebang (#!).
 * 
 * @param filename Path to the file.
 * @param interpreter Output buffer for the interpreter path.
 * @param interp_size Size of the output buffer.
 * @return 0 if shebang found, -1 otherwise.
 */
int check_shebang(const char* filename, char* interpreter, int interp_size) {
    int fd = fs_open(filename, O_RDONLY);
    if (fd < 0) return -1;

    char shebang[128];
    int bytes_read = fs_read(fd, shebang, sizeof(shebang) - 1);
    fs_close(fd);

    if (bytes_read > 2 && shebang[0] == '#' && shebang[1] == '!') {
        /* Extract interpreter path from #! line */
        int i = 2;
        int out = 0;
        /* Skip leading whitespace after #! */
        while (i < bytes_read && shebang[i] == ' ') i++;
        
        while (i < bytes_read && out < interp_size - 1 &&
               shebang[i] != '\n' && shebang[i] != '\r' && shebang[i] != ' ') {
            interpreter[out++] = shebang[i];
            i++;
        }
        interpreter[out] = '\0';
        return 0;
    }

    return -1;
}

/**
 * @brief Compile and execute C source code in-memory using TCC.
 * 
 * @param c_source Null-terminated string containing C code.
 * @param argv Arguments to pass to the compiled program.
 * @return 0 on success, -1 on failure.
 */
int exec_c_code(const char* c_source, char** argv) {
    tcc_state_t* tcc = tcc_new();
    if (!tcc) return -1;
    
    if (tcc_compile_string(tcc, c_source) < 0) {
        tcc_delete(tcc);
        return -1;
    }
    
    uint8_t* elf_data;
    uint32_t elf_size;
    if (tcc_output_memory(tcc, &elf_data, &elf_size) < 0) {
        tcc_delete(tcc);
        return -1;
    }
    
    char* dummy_argv[] = {"a.out", NULL};
    char** actual_argv = argv ? argv : dummy_argv;
    int argc = 0;
    while (actual_argv[argc]) argc++;

    /* Load the compiled ELF into memory */
    uint32_t entry = elf_load_with_args(elf_data, argc, actual_argv);
    if (entry == 0) {
        tcc_delete(tcc);
        return -1;
    }
    
    /* Setup user-mode stack - MUST map with user-accessible flags */
    uint32_t user_stack = 0x800000;
    uint32_t new_esp;
    setup_user_stack(user_stack, argc, actual_argv, &new_esp);
    
    /* Map user stack pages with user-mode access (flags = 7 = Present + RW + User) */
    for (uint32_t stack_page = user_stack - 0x1000; stack_page >= 0x700000; stack_page -= 0x1000) {
        void* frame = pmm_alloc_block();
        if (!frame) {
            terminal_writestring("exec: Failed to allocate stack frame\n");
            return -1;
        }
        map_page(frame, (void*)stack_page, 7); /* User + RW + Present */
    }
    
    /* Ensure the TSS is updated with the kernel stack for syscalls */
    extern uint32_t kernel_stack_top;
    set_kernel_stack(kernel_stack_top);
    
    /* Jump to user mode at the ELF entry point */
    jump_to_user_mode(entry, new_esp);
    
    tcc_delete(tcc);
    return 0;
}

/**
 * @brief Main entry point for executing a file.
 * 
 * Supports both binary ELF files and shebang scripts (compiled via TCC).
 * 
 * @param filename Path to the executable.
 * @param argv Argument vector.
 * @param envp Environment vector (unused).
 * @return 0 on success, -1 on failure.
 */
int execve_file(const char* filename, char** argv, char** envp) {
    (void)envp;
    if (!filename) {
        terminal_writestring("execve: No filename specified\n");
        return -1;
    }
    
    log_serial("execve: ");
    log_serial(filename);
    log_serial("\n");
    
    char* dummy_argv[] = {(char*)filename, NULL};
    char** actual_argv = argv ? argv : dummy_argv;

    /* Handle Shebang scripts (#! /usr/bin/tcc) */
    char interpreter[128];
    if (check_shebang(filename, interpreter, sizeof(interpreter)) == 0) {
        if (strcmp(interpreter, "/usr/bin/tcc") == 0 || strcmp(interpreter, "tcc") == 0) {
            int fd = fs_open(filename, O_RDONLY);
            if (fd < 0) return -1;

            uint32_t file_size = fs_seek(fd, 0, 2);
            fs_seek(fd, 0, 0);

            char* source = (char*)kmalloc(file_size + 1);
            if (!source) {
                fs_close(fd);
                return -1;
            }

            fs_read(fd, source, file_size);
            source[file_size] = '\0';
            fs_close(fd);

            int result = exec_c_code(source, actual_argv);
            kfree(source);
            return result;
        }
    }
    
    /* Regular ELF execution */
    int fd = fs_open(filename, O_RDONLY);
    if (fd < 0) {
        terminal_writestring("execve: File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        return -1;
    }
    
    uint32_t file_size = fs_seek(fd, 0, 2);
    fs_seek(fd, 0, 0);
    
    uint8_t* file_data = (uint8_t*)kmalloc(file_size);
    if (!file_data) {
        fs_close(fd);
        return -1;
    }
    
    fs_read(fd, file_data, file_size);
    fs_close(fd);
    
    /* Verify ELF Magic */
    if (file_data[0] == 0x7F && file_data[1] == 'E' &&
        file_data[2] == 'L' && file_data[3] == 'F') {
            
        int argc = 0;
        while (actual_argv[argc]) argc++;
            
        uint32_t entry = elf_load_with_args(file_data, argc, actual_argv);
        if (entry == 0) {
            kfree(file_data);
            return -1;
        }

        uint32_t user_stack = 0x800000;
        uint32_t new_esp;
        setup_user_stack(user_stack, argc, actual_argv, &new_esp);

        for (uint32_t stack_page = user_stack - 0x1000; stack_page >= 0x700000; stack_page -= 0x1000) {
            void* frame = pmm_alloc_block();
            if (!frame) {
                terminal_writestring("exec: Failed to allocate stack frame\n");
                kfree(file_data);
                return -1;
            }
            map_page(frame, (void*)stack_page, 7);
        }

        extern uint32_t kernel_stack_top;
        set_kernel_stack(kernel_stack_top);

        jump_to_user_mode(entry, new_esp);

        kfree(file_data);
        return 0;
    } else {
        terminal_writestring("execve: Not an ELF file: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        kfree(file_data);
        return -1;
    }
}
