#include "exec.h"
#include "fs.h"
#include "elf.h"
#include "tcc.h"
#include "kheap.h"
#include "string.h"
#include "syscall.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);
extern void set_kernel_stack(uint32_t stack);
extern void log_serial(const char* str);
extern void log_hex_serial(uint32_t n);
extern void int_to_string(uint32_t val, char* str);

// Simple int to string conversion
static void int_to_string_local(uint32_t val, char* str) {
    int i = 0;
    if (val == 0) { str[0] = '0'; str[1] = '\0'; return; }
    while (val > 0) { str[i++] = val % 10 + '0'; val /= 10; }
    str[i] = '\0';
    // Reverse string
    for (int j = 0; j < i/2; j++) {
        char temp = str[j]; str[j] = str[i-1-j]; str[i-1-j] = temp;
    }
}

int check_shebang(const char* filename, char* interpreter, int interp_size) {
    int fd = fs_open(filename, O_RDONLY);
    if (fd < 0) return -1;
    
    char shebang[256];
    int bytes_read = fs_read(fd, shebang, sizeof(shebang) - 1);
    fs_close(fd);
    
    if (bytes_read > 2 && shebang[0] == '#' && shebang[1] == '!') {
        // Extract interpreter path
        int i = 2;
        while (i < bytes_read && i < interp_size - 1 && 
               shebang[i] != '\n' && shebang[i] != '\r' && shebang[i] != ' ') {
            interpreter[i-2] = shebang[i];
            i++;
        }
        interpreter[i-2] = '\0';
        return 0;
    }
    
    return -1; // No shebang found
}

int exec_c_code(const char* c_source, char** argv) {
    tcc_state_t* tcc = tcc_new();
    if (!tcc) {
        return -1;
    }
    
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
    
    // Ensure we have a valid argv
    char* dummy_argv[] = {"a.out", NULL};
    char** actual_argv = argv ? argv : dummy_argv;
    int argc = 0;
    while (actual_argv[argc]) argc++;

    // Load and execute the compiled ELF
    uint32_t entry = elf_load_with_args(elf_data, argc, actual_argv);
    if (entry == 0) {
        tcc_delete(tcc);
        return -1;
    }
    
    // Set up user stack and jump to user mode
    uint32_t user_stack = 0x800000; // User stack address
    uint32_t new_esp;
    
    setup_user_stack(user_stack, argc, actual_argv, &new_esp);
    
    // Set kernel stack for system calls
    extern uint32_t kernel_stack_top;
    set_kernel_stack(kernel_stack_top);
    
    // Jump to user mode
    jump_to_user_mode(entry, new_esp);
    
    tcc_delete(tcc);
    return 0;
}

int execve_file(const char* filename, char** argv, char** envp) {
    if (!filename) {
        terminal_writestring("execve: No filename specified\n");
        return -1;
    }
    
    log_serial("execve: ");
    log_serial(filename);
    log_serial("\n");
    
    // Check for shebang
    char interpreter[128];
    if (check_shebang(filename, interpreter, sizeof(interpreter)) == 0) {
        terminal_writestring("Found shebang: ");
        terminal_writestring(interpreter);
        terminal_writestring("\n");
        
        // If it's our C interpreter, compile and run
        if (strcmp(interpreter, "/usr/bin/tcc") == 0 || 
            strcmp(interpreter, "tcc") == 0) {
            
            // Read the C source file
            int fd = fs_open(filename, O_RDONLY);
            if (fd < 0) {
                terminal_writestring("Failed to open C source file\n");
                return -1;
            }
            
            // Read entire file
            uint32_t file_size = fs_seek(fd, 0, 2); // Seek to end
            fs_seek(fd, 0, 0); // Seek back to start
            
            char* source = (char*)kmalloc(file_size + 1);
            if (!source) {
                terminal_writestring("Failed to allocate memory for source\n");
                fs_close(fd);
                return -1;
            }
            
            int bytes_read = fs_read(fd, source, file_size);
            source[bytes_read] = '\0';
            fs_close(fd);
            
            // Execute C code
            int result = exec_c_code(source, argv);
            kfree(source);
            return result;
        }
    }
    
    // Regular ELF execution
    int fd = fs_open(filename, O_RDONLY);
    if (fd < 0) {
        terminal_writestring("execve: File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        return -1;
    }
    
    // Read file to check if it's ELF
    uint32_t file_size = fs_seek(fd, 0, 2); // Seek to end
    fs_seek(fd, 0, 0); // Seek back to start
    
    uint8_t* file_data = (uint8_t*)kmalloc(file_size);
    if (!file_data) {
        terminal_writestring("execve: Memory allocation failed\n");
        fs_close(fd);
        return -1;
    }
    
    int bytes_read = fs_read(fd, file_data, file_size);
    fs_close(fd);
    
    if (bytes_read < 4) {
        terminal_writestring("execve: Invalid file\n");
        kfree(file_data);
        return -1;
    }
    
            // Check ELF magic
        if (file_data[0] == 0x7F && file_data[1] == 'E' && 
            file_data[2] == 'L' && file_data[3] == 'F') {
            
            // Calculate argc (skip program name)
            int argc = 0;
            if (argv) {
                while (argv[argc]) argc++;
            }
            
            uint32_t entry = elf_load_with_args(file_data, argc, argv);
            if (entry == 0) {
                kfree(file_data);
                return -1;
            }
            
            // Set up user stack
            uint32_t user_stack = 0x800000;
            uint32_t new_esp;
            setup_user_stack(user_stack, argc, argv, &new_esp);
            
            // Set kernel stack for system calls
            extern uint32_t kernel_stack_top;
            set_kernel_stack(kernel_stack_top);
            
            // Jump to user mode
            jump_to_user_mode(entry, new_esp);
            
            kfree(file_data);
            return 0;
        } else {        terminal_writestring("execve: Not an ELF file: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        kfree(file_data);
        return -1;
    }
}