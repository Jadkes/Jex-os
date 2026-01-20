#include "elf.h"
#include "kheap.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);

uint32_t elf_load(uint8_t* elf_data) {
    Elf32_Ehdr* header = (Elf32_Ehdr*)elf_data;

    // Verify ELF Magic
    if (header->e_ident[0] != 0x7F ||
        header->e_ident[1] != 'E'  ||
        header->e_ident[2] != 'L'  ||
        header->e_ident[3] != 'F') {
        terminal_writestring("Invalid ELF Magic!\n");
        return 0;
    }

    // Load Program Headers
    for (int i = 0; i < header->e_phnum; i++) {
        Elf32_Phdr* phdr = (Elf32_Phdr*)(elf_data + header->e_phoff + (i * header->e_phentsize));

        if (phdr->p_type == PT_LOAD) {
            // Copy segment data to its virtual address
            // Note: Since we are identity mapped, virtual == physical
            memcpy((void*)phdr->p_vaddr, elf_data + phdr->p_offset, phdr->p_filesz);

            // Zero out remaining memory if memsz > filesz (for .bss)
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((void*)(phdr->p_vaddr + phdr->p_filesz), 0, phdr->p_memsz - phdr->p_filesz);
            }
        }
    }

    return header->e_entry;
}
