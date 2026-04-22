/**
 * @file elf.c
 * @brief ELF binary loader for user-space programs.
 *
 * This file handles parsing the ELF file format, mapping program segments
 * into memory, and setting up the initial user-mode stack with arguments.
 */

#include "elf.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "paging.h"
#include "terminal.h"
#include "serial.h"
#include <stddef.h>

/**
 * @brief Apply a relocation to a specific address in the loaded binary.
 * 
 * @param reloc_addr The virtual address to relocate.
 * @param sym_addr The resolved symbol address.
 * @param type Relocation type (R_386_32 or R_386_PC32).
 */
static void apply_relocation(uint32_t reloc_addr, uint32_t sym_addr, uint32_t type) {
    switch (type) {
        case R_386_32: /* Direct absolute */
            *(uint32_t*)reloc_addr += sym_addr;
            break;
        case R_386_PC32: /* PC relative */
            *(uint32_t*)reloc_addr += sym_addr - reloc_addr;
            break;
        default:
            terminal_writestring("Unsupported relocation type\n");
            break;
    }
}

/**
 * @brief Process ELF relocation sections.
 */
static void handle_relocations(uint8_t* elf_data, Elf32_Ehdr* header) {
    Elf32_Shdr* shdr = (Elf32_Shdr*)(elf_data + header->e_shoff);
    Elf32_Shdr* shstrtab = &shdr[header->e_shstrndx];
    char* section_names = (char*)(elf_data + shstrtab->sh_offset);

    for (int i = 0; i < (int)header->e_shnum; i++) {
        char* sec_name = section_names + shdr[i].sh_name;
        
        if ((shdr[i].sh_type == SHT_REL || shdr[i].sh_type == SHT_RELA) &&
            (strstr(sec_name, ".rel.") != NULL)) {
            
            Elf32_Rel* rels = (Elf32_Rel*)(elf_data + shdr[i].sh_offset);
            int rel_count = shdr[i].sh_size / shdr[i].sh_entsize;
            
            for (int j = 0; j < rel_count; j++) {
                uint32_t reloc_addr = rels[j].r_offset;
                uint32_t sym_info = rels[j].r_info;
                
                uint32_t rel_type = sym_info & 0xFF;
                
                /* For now, assume a simplified symbol resolution */
                uint32_t sym_addr = header->e_entry; 
                
                apply_relocation(reloc_addr, sym_addr, rel_type);
            }
        }
    }
}

/**
 * @brief Load an ELF binary into memory without arguments.
 */
uint32_t elf_load(uint8_t* elf_data) {
    return elf_load_with_args(elf_data, 0, NULL);
}

/**
 * @brief Main ELF loading logic.
 * 
 * Maps PT_LOAD segments into memory and clears BSS.
 * 
 * @param elf_data Pointer to the raw ELF file in memory.
 * @param argc Number of arguments.
 * @param argv Argument vector.
 * @return The virtual entry point address of the loaded program.
 */
uint32_t elf_load_with_args(uint8_t* elf_data, int argc, char** argv) {
    Elf32_Ehdr* header = (Elf32_Ehdr*)elf_data;

    /* Verify ELF Magic */
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E'  ||
        header->e_ident[2] != 'L'  || header->e_ident[3] != 'F') {
        terminal_writestring("Invalid ELF Magic!\n");
        return 0;
    }

    /* Verify machine architecture */
    if (header->e_ident[EI_CLASS] != ELFCLASS32 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_machine != EM_386) {
        terminal_writestring("Unsupported ELF format\n");
        return 0;
    }

    /* Iterate through Program Headers to find loadable segments */
    for (int i = 0; i < header->e_phnum; i++) {
        Elf32_Phdr* phdr = (Elf32_Phdr*)(elf_data + header->e_phoff + (i * header->e_phentsize));

        if (phdr->p_type == PT_LOAD) {
            /* Allocate and map virtual pages for this segment */
            uint32_t start_page = phdr->p_vaddr & 0xFFFFF000;
            uint32_t end_page = (phdr->p_vaddr + phdr->p_memsz + 4095) & 0xFFFFF000;
            
            for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
                void* frame = pmm_alloc_block();
                if (!frame) return 0;
                map_page(frame, (void*)addr, 7); /* User + RW + Present */
            }
            
            /* Copy segment data from the ELF image to its virtual address */
            memcpy((void*)phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz);

            /* Clear BSS (memory size > file size) */
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((void*)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
            }
        }
    }

    /* Skip relocations for ET_EXEC (directly linked executables)
     * TCC-generated ELF binaries are ET_EXEC with no relocations needed */
    if (header->e_type == ET_EXEC) {
        (void)argc; (void)argv;
        return header->e_entry;
    }

    /* Handle relocations if this is a relocatable object */
    if (header->e_type == ET_REL || (header->e_flags & EF_RELOC)) {
        handle_relocations(elf_data, header);
    }

    (void)argc; (void)argv;
    return header->e_entry;
}

/**
 * @brief Sets up the initial user stack with argc and argv.
 * 
 * Pushes strings and pointers onto the top of the user stack so that
 * 'main(int argc, char** argv)' can receive them.
 */
void setup_user_stack(uint32_t stack_top, int argc, char** argv, uint32_t* new_esp) {
    uint32_t* esp = (uint32_t*)stack_top;
    uint32_t argv_pointers[16]; 
    int actual_argc = 0;
    
    /* Copy argument strings onto the stack */
    for (int i = 0; i < argc && i < 16; i++) {
        if (argv[i]) {
            int len = strlen(argv[i]) + 1;
            esp = (uint32_t*)((uint32_t)esp - ((len + 3) & ~3)); /* Align to 4 bytes */
            strcpy((char*)esp, argv[i]);
            argv_pointers[i] = (uint32_t)esp;
            actual_argc++;
        }
    }
    
    /* Push the argv array (pointers to the strings above) */
    *--esp = 0; /* NULL terminator for argv */
    for (int i = actual_argc - 1; i >= 0; i--) {
        *--esp = argv_pointers[i];
    }
    
    uint32_t argv_start = (uint32_t)esp;
    *--esp = argv_start; /* Pointer to argv[0] */
    *--esp = actual_argc; /* argc */
    
    *new_esp = (uint32_t)esp;
}
