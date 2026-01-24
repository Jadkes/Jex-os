#include "elf.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "paging.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void log_serial(const char* str);
extern void log_hex_serial(uint32_t val);

// Helper function for relocation
static void apply_relocation(uint32_t reloc_addr, uint32_t sym_addr, uint32_t type) {
    switch (type) {
        case R_386_32: // Direct absolute
            *(uint32_t*)reloc_addr += sym_addr;
            break;
        case R_386_PC32: // PC relative
            *(uint32_t*)reloc_addr += sym_addr - reloc_addr;
            break;
        default:
            terminal_writestring("Unsupported relocation type\n");
            break;
    }
}

static void handle_relocations(uint8_t* elf_data, Elf32_Ehdr* header) {
    // Find relocation sections in section headers
    Elf32_Shdr* shdr = (Elf32_Shdr*)(elf_data + header->e_shoff);
    Elf32_Shdr* shstrtab = &shdr[header->e_shstrndx];
    char* section_names = (char*)(elf_data + shstrtab->sh_offset);

    for (int i = 0; i < header->e_shnum; i++) {
        char* sec_name = section_names + shdr[i].sh_name;
        
        // Look for .rel.text and other relocation sections
        if ((shdr[i].sh_type == SHT_REL || shdr[i].sh_type == SHT_RELA) &&
            (strstr(sec_name, ".rel.") != NULL)) {
            
            Elf32_Rel* rels = (Elf32_Rel*)(elf_data + shdr[i].sh_offset);
            int rel_count = shdr[i].sh_size / shdr[i].sh_entsize;
            
            for (int j = 0; j < rel_count; j++) {
                uint32_t reloc_addr = rels[j].r_offset;
                uint32_t sym_info = rels[j].r_info;
                
                uint32_t sym_index = sym_info >> 8;
                uint32_t rel_type = sym_info & 0xFF;
                
                // For now, assume all symbols are in the same section
                uint32_t sym_addr = header->e_entry; // Simplified symbol resolution
                
                apply_relocation(reloc_addr, sym_addr, rel_type);
            }
        }
    }
}

uint32_t elf_load(uint8_t* elf_data) {
    return elf_load_with_args(elf_data, 0, NULL);
}

uint32_t elf_load_with_args(uint8_t* elf_data, int argc, char** argv) {
    Elf32_Ehdr* header = (Elf32_Ehdr*)elf_data;

    // Verify ELF Magic
    if (header->e_ident[0] != 0x7F ||
        header->e_ident[1] != 'E'  ||
        header->e_ident[2] != 'L'  ||
        header->e_ident[3] != 'F') {
        terminal_writestring("Invalid ELF Magic!\n");
        return 0;
    }

    // Check for 32-bit, little endian, i386
    if (header->e_ident[EI_CLASS] != ELFCLASS32 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB ||
        header->e_machine != EM_386) {
        terminal_writestring("Unsupported ELF format\n");
        return 0;
    }

    // Load Program Headers
    for (int i = 0; i < header->e_phnum; i++) {
        Elf32_Phdr* phdr = (Elf32_Phdr*)(elf_data + header->e_phoff + (i * header->e_phentsize));

        if (phdr->p_type == PT_LOAD) {
            // Allocate and map pages for this segment
            uint32_t start_page = phdr->p_vaddr & 0xFFFFF000;
            uint32_t end_page = (phdr->p_vaddr + phdr->p_memsz + 4095) & 0xFFFFF000;
            
            for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
                void* frame = pmm_alloc_block();
                if (!frame) {
                    terminal_writestring("ELF Load: Out of Memory!\n");
                    return 0;
                }
                map_page(frame, (void*)addr, 3); // User + RW + Present
            }
            
            // Copy segment data to its virtual address
            memcpy((void*)phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz);

            // Enhanced .bss handling - zero out remaining memory
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((void*)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
                log_serial("BSS section zeroed: ");
                log_hex_serial(phdr->p_memsz - phdr->p_filesz);
                log_serial(" bytes\n");
            }
        }
    }

    // Handle relocations if present
    if (header->e_type == ET_REL || (header->e_flags & EF_RELOC)) {
        handle_relocations(elf_data, header);
    }

    return header->e_entry;
}

void setup_user_stack(uint32_t stack_top, int argc, char** argv, uint32_t* new_esp) {
    uint32_t* esp = (uint32_t*)stack_top;
    
    // Push argv strings (simplified - in real implementation we'd copy strings)
    uint32_t argv_pointers[16]; // Max 16 args for now
    int actual_argc = 0;
    
    for (int i = 0; i < argc && i < 16; i++) {
        if (argv[i]) {
            // Copy string to stack
            int len = strlen(argv[i]) + 1;
            esp -= (len + 3) / 4; // Align to 4 bytes
            strcpy((char*)esp, argv[i]);
            argv_pointers[i] = (uint32_t)esp;
            actual_argc++;
        }
    }
    
    // Push argv array (NULL terminated)
    for (int i = actual_argc; i >= 0; i--) {
        *--esp = argv_pointers[i];
    }
    
    // Push argc
    *--esp = actual_argc;
    
    *new_esp = (uint32_t)esp;
}