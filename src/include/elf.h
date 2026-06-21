/**
 * @file elf.h
 * @brief Executable and Linkable Format (ELF) definitions for 32-bit x86.
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define EI_NIDENT 16

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

/**
 * @struct Elf32_Ehdr
 * @brief ELF Header (32-bit).
 */
typedef struct {
    unsigned char e_ident[EI_NIDENT]; /**< ELF Identification bytes. */
    Elf32_Half e_type;      /**< Object file type. */
    Elf32_Half e_machine;   /**< Architecture (e.g., EM_386). */
    Elf32_Word e_version;   /**< Object file version. */
    Elf32_Addr e_entry;     /**< Entry point virtual address. */
    Elf32_Off e_phoff;      /**< Program header table file offset. */
    Elf32_Off e_shoff;      /**< Section header table file offset. */
    Elf32_Word e_flags;     /**< Processor-specific flags. */
    Elf32_Half e_ehsize;    /**< ELF header size in bytes. */
    Elf32_Half e_phentsize; /**< Program header table entry size. */
    Elf32_Half e_phnum;     /**< Program header table entry count. */
    Elf32_Half e_shentsize; /**< Section header table entry size. */
    Elf32_Half e_shnum;     /**< Section header table entry count. */
    Elf32_Half e_shstrndx;  /**< Section header string table index. */
} __attribute__((packed)) Elf32_Ehdr;

/**
 * @struct Elf32_Phdr
 * @brief ELF Program Header (Segment).
 */
typedef struct {
    Elf32_Word p_type;   /**< Segment type (e.g., PT_LOAD). */
    Elf32_Off p_offset;  /**< Segment file offset. */
    Elf32_Addr p_vaddr;  /**< Segment virtual address. */
    Elf32_Addr p_paddr;  /**< Segment physical address (ignored). */
    Elf32_Word p_filesz; /**< Segment size in file. */
    Elf32_Word p_memsz;  /**< Segment size in memory. */
    Elf32_Word p_flags;  /**< Segment flags (R/W/X). */
    Elf32_Word p_align;  /**< Segment alignment. */
} __attribute__((packed)) Elf32_Phdr;

/**
 * @struct Elf32_Shdr
 * @brief ELF Section Header.
 */
typedef struct {
    uint32_t sh_name;      /**< Section name (string table offset). */
    uint32_t sh_type;      /**< Section type. */
    uint32_t sh_flags;     /**< Section attributes. */
    Elf32_Addr sh_addr;    /**< Section virtual address. */
    Elf32_Off sh_offset;   /**< Section file offset. */
    uint32_t sh_size;      /**< Section size. */
    uint32_t sh_link;      /**< Link to another section. */
    uint32_t sh_info;      /**< Additional section information. */
    uint32_t sh_addralign; /**< Section alignment. */
    uint32_t sh_entsize;   /**< Entry size if section holds a table. */
} __attribute__((packed)) Elf32_Shdr;

/**
 * @struct Elf32_Rel
 * @brief ELF Relocation Entry (without addend).
 */
typedef struct {
    Elf32_Addr r_offset; /**< Address to be relocated. */
    uint32_t r_info;     /**< Relocation type and symbol index. */
} __attribute__((packed)) Elf32_Rel;

/* ELF Identification Indexes */
#define EI_CLASS 4
#define EI_DATA 5

/* ELF Constants */
#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define EM_386 3
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN  3
#define EF_RELOC 0x1

/* Section Types */
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_REL 9

/* Program Header Types */
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6

/* Symbol Table Entry */
typedef struct {
    uint32_t st_name;     /**< Index into symbol string table. */
    Elf32_Addr st_value;  /**< Symbol value (address or offset). */
    uint32_t st_size;     /**< Symbol size. */
    unsigned char st_info; /**< Symbol type and binding. */
    unsigned char st_other; /**< Symbol visibility. */
    Elf32_Half st_shndx;  /**< Section header index. */
} __attribute__((packed)) Elf32_Sym;

/* Symbol bindings (ELF_ST_BIND) */
#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

/* Symbol types (ELF_ST_TYPE) */
#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3

/* x86 Relocation Types */
#define R_386_32   1  /**< Direct absolute. */
#define R_386_PC32 2  /**< PC relative. */
#define R_386_GOT32 3 /**< GOT relative. */

/**
 * @brief Load an ELF binary into memory.
 */
uint32_t elf_load(uint8_t* elf_data);

/**
 * @brief Load an ELF binary and prepare it for execution with arguments.
 */
uint32_t elf_load_with_args(uint8_t* elf_data, int argc, char** argv);

/**
 * @brief Set up the user stack with command line arguments.
 */
void setup_user_stack(uint32_t stack_top, int argc, char** argv, uint32_t* new_esp);

#endif // ELF_H
