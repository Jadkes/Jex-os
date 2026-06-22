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
#include "task.h"
#include <jexos/errno.h>
#include <stddef.h>

/* Forward declarations for architecture-specific assembly functions */
extern void jump_to_user_mode(uint32_t entry, uint32_t stack);

/* User stack configuration — 1 MB at 0x800000 */
#define USER_STACK_BASE     0x800000
#define USER_STACK_PAGES    256
#define USER_STACK_LOW      0x700000 /* USER_STACK_BASE - USER_STACK_PAGES * 0x1000 */

/* Page-table flags for user stack: Present | Writable | User */
#define USER_STACK_FLAGS    7

/**
 * @brief Map user-mode stack pages with proper permissions.
 *
 * Allocates and maps USER_STACK_PAGES worth of 4 KB frames below
 * USER_STACK_BASE so the user program has a usable stack.
 *
 * @return 0 on success, -1 on allocation failure.
 */
static int map_user_stack(void)
{
    int allocated = 0;
    void* frames[USER_STACK_PAGES];

    for (uint32_t page = USER_STACK_BASE - 0x1000, i = 0;
         page >= USER_STACK_LOW && i < USER_STACK_PAGES;
         page -= 0x1000, i++) {
        void* frame = pmm_alloc_block();
        if (!frame) {
            /* OOM — free pages mapped so far */
            for (int j = 0; j < allocated; j++)
                pmm_free_block(frames[j]);
            terminal_writestring("exec: Failed to allocate stack frame\n");
            return -ENOMEM;
        }
        frames[allocated++] = frame;
        if (map_page(frame, (void*)page, USER_STACK_FLAGS) < 0) {
            for (int j = 0; j < allocated; j++)
                pmm_free_block(frames[j]);
            terminal_writestring("exec: Failed to map stack page\n");
            return -ENOMEM;
        }
    }
    return 0;
}

/**
 * @brief Inspect the first bytes of a file to check for a shebang (#!).
 *
 * @param filename Path to the file.
 * @param interpreter Output buffer for the interpreter path.
 * @param interp_size Size of the output buffer.
 * @return 0 if shebang found, -1 otherwise.
 */
int check_shebang(const char* filename, char* interpreter, int interp_size)
{
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
int exec_c_code(const char* c_source, char** argv)
{
    tcc_state_t* tcc = tcc_new();
    if (!tcc) return -ENOMEM;

    if (tcc_compile_string(tcc, c_source) < 0) {
        tcc_delete(tcc);
        return -ENOEXEC;
    }

    uint8_t* elf_data;
    uint32_t elf_size;
    if (tcc_output_memory(tcc, &elf_data, &elf_size) < 0) {
        tcc_delete(tcc);
        return -ENOMEM;
    }

    char* dummy_argv[] = {"a.out", NULL};
    char** actual_argv = argv ? argv : dummy_argv;
    int argc = 0;
    while (actual_argv[argc]) argc++;

    /* Load the compiled ELF into memory */
    uint32_t entry = elf_load_with_args(elf_data, argc, actual_argv);
    if (entry == 0) {
        tcc_delete(tcc);
        return -ENOEXEC;
    }

    /* Map user stack BEFORE writing to it */
    if (map_user_stack() < 0) {
        tcc_delete(tcc);
        return -ENOMEM;
    }

    /* Setup user-mode stack with arguments */
    uint32_t new_esp;
    setup_user_stack(USER_STACK_BASE, argc, actual_argv, &new_esp);

    /* TSS.ESP0 is already set per-task by task_switch — no need to override.
     * Jump to user mode at the ELF entry point — never returns */
    jump_to_user_mode(entry, new_esp);

    /* Not reached */
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
int execve_file(const char* filename, char** argv, char** envp)
{
    (void)envp;
    if (!filename) {
        terminal_writestring("execve: No filename specified\n");
        return -EINVAL;
    }

    log_serial("execve: ");
    log_serial(filename);
    log_serial("\n");
    /* Log current task's page directory vs CR3 */
    log_serial("[EXEC] current_task pid=");
    log_hex_serial(current_task->id);
    log_serial(" page_directory=0x");
    log_hex_serial((uint32_t)current_task->page_directory);
    log_serial(" CR3=0x");
    {
        uint32_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        log_hex_serial(cr3);
    }
    log_serial("\n");

    char* dummy_argv[] = {(char*)filename, NULL};
    char** actual_argv = argv ? argv : dummy_argv;

    /* Handle Shebang scripts (#! /usr/bin/tcc) */
    char interpreter[128];
    if (check_shebang(filename, interpreter, sizeof(interpreter)) == 0) {
        if (strcmp(interpreter, "/usr/bin/tcc") == 0 || strcmp(interpreter, "tcc") == 0) {
            int fd = fs_open(filename, O_RDONLY);
            if (fd < 0) return fd;  /* propagate -ENOENT / -EACCES from fs_open */

            uint32_t file_size = fs_seek(fd, 0, 2);
            fs_seek(fd, 0, 0);

            char* source = (char*)kmalloc(file_size + 1);
            if (!source) {
                fs_close(fd);
                return -ENOMEM;
            }

            int read_size = fs_read(fd, source, file_size);
            if (read_size < 0 || (uint32_t)read_size != file_size) {
                kfree(source);
                fs_close(fd);
                return -EIO;
            }
            source[read_size] = '\0';
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
        return fd;  /* propagate errno from fs_open */
    }

    uint32_t file_size = fs_seek(fd, 0, 2);
    fs_seek(fd, 0, 0);

    uint8_t* file_data = (uint8_t*)kmalloc(file_size);
    if (!file_data) {
        fs_close(fd);
        return -ENOMEM;
    }

    int bytes_read = fs_read(fd, file_data, file_size);
    if (bytes_read < 0 || (uint32_t)bytes_read != file_size) {
        kfree(file_data);
        fs_close(fd);
        return -EIO;
    }
    fs_close(fd);

    /* Verify ELF Magic */
    if (file_data[0] == 0x7F && file_data[1] == 'E' &&
        file_data[2] == 'L' && file_data[3] == 'F') {

        int argc = 0;
        while (actual_argv[argc]) argc++;

        uint32_t entry = elf_load_with_args(file_data, argc, actual_argv);
        log_serial("[EXEC] entry=0x");
        log_hex_serial(entry);
        log_serial("\n");
        if (entry == 0) {
            kfree(file_data);
            return -ENOEXEC;
        }

        /* Map user stack BEFORE writing to it */
        log_serial("[EXEC] mapping user stack\n");
        if (map_user_stack() < 0) {
            kfree(file_data);
            return -ENOMEM;
        }

        /* Setup user-mode stack with arguments */
        uint32_t new_esp;
        setup_user_stack(USER_STACK_BASE, argc, actual_argv, &new_esp);

        /* Print TSS.ESP0 and current task info for crash debugging */
        {
            extern tss_entry_t tss_entry;
            extern volatile task_t* current_task;
            log_serial("[EXEC] TSS.ESP0=0x");
            log_hex_serial(tss_entry.esp0);
            log_serial(" task.kstack_top=0x");
            if (current_task && current_task->kstack)
                log_hex_serial(current_task->kstack + 8192);
            else
                log_serial("NULL");
            log_serial(" pid=");
            log_hex_serial(current_task ? current_task->id : 0);
            log_serial("\n");
        }

        /* TSS.ESP0 already set per-task by task_switch */
        log_serial("[EXEC] jumping to user mode\n");

        /* Jump to user mode at the ELF entry point — never returns */
        jump_to_user_mode(entry, new_esp);

        /* Not reached */
        kfree(file_data);
        return 0;
    } else {
        terminal_writestring("execve: Not an ELF file: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        kfree(file_data);
        return -ENOEXEC;
    }
}
