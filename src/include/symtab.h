/**
 * @file symtab.h
 * @brief Symbol table for Tiny C Compiler (TCC).
 *
 * Implements a fixed-size array symbol table with scope management.
 * Used for tracking variables, functions, and their types during compilation.
 */

#ifndef SYMTAB_H
#define SYMTAB_H

#include <stdint.h>

/** @brief Maximum symbol name length (including null terminator) */
#define SYM_NAME_MAX 32

/** @brief Maximum number of symbols in the table */
#define SYM_TABLE_SIZE 256

/**
 * @brief Symbol types for the compiler.
 */
typedef enum {
    SYM_INT,    /**< int type */
    SYM_CHAR,   /**< char type */
    SYM_VOID,   /**< void type */
    SYM_PTR,    /**< pointer type */
    SYM_FUNC,   /**< function type */
    SYM_ARRAY,
    SYM_UNDEF   /**< undefined/error type */
} sym_type_t;

/**
 * @brief A single symbol table entry.
 */
typedef struct {
    char name[SYM_NAME_MAX];  /**< Symbol name (identifier) */
    sym_type_t type;           /**< Data type of the symbol */
    sym_type_t base_type;       /**< For pointers: type of pointed-to data */
    int offset;                /**< Stack offset for locals, 0 for global */
    int scope_level;            /**< Scope level (0=global, 1+=local) */
    int array_size;             /**< For arrays: number of elements; 0 for non-arrays */
} symbol_t;

/**
 * @brief Symbol table state.
 *
 * Uses a fixed-size array (no dynamic allocation).
 * Tracks current scope and stack offset for local variables.
 */
typedef struct {
    symbol_t entries[SYM_TABLE_SIZE];  /**< Fixed array of symbols */
    int current_scope;                  /**< Current scope level (0=global) */
    int current_offset;                 /**< Current stack offset for locals */
    int next_free;                      /**< Next free slot hint (optimization) */
} symtab_t;

/* Symbol Table API Functions */

/**
 * @brief Initialize the symbol table.
 * @param tab Pointer to symbol table to initialize.
 *
 * Sets all entries to empty, scope=0, offset=0.
 */
void symtab_init(symtab_t* tab);

/**
 * @brief Add a symbol to the table.
 * @param tab Pointer to symbol table.
 * @param name Symbol name (max SYM_NAME_MAX-1 chars).
 * @param type Data type of the symbol.
 * @param offset Stack offset (used for local variables).
 * @param scope_level Scope level where symbol is defined.
 * @return 0 on success, -1 on failure (table full or invalid input).
 *
 * Finds first free slot and adds the symbol.
 * Updates next_free hint for faster subsequent adds.
 */
int symtab_add(symtab_t* tab, const char* name, sym_type_t type, int offset, int scope_level);

/**
 * @brief Add an array symbol to the table.
 * @param tab Pointer to symbol table.
 * @param name Symbol name.
 * @param type Element type of the array.
 * @param offset Stack offset.
 * @param scope_level Scope level.
 * @param array_size Number of elements in the array.
 * @return 0 on success, -1 on failure.
 */
int symtab_add_array(symtab_t* tab, const char* name, sym_type_t type, int offset, int scope_level, int array_size);

/**
 * @brief Look up a symbol by name.
 * @param tab Pointer to symbol table.
 * @param name Symbol name to search for.
 * @return Pointer to symbol_t if found, NULL if not found.
 *
 * Performs linear search from newest to oldest entry.
 * Returns the most recently added symbol with matching name.
 */
symbol_t* symtab_lookup(symtab_t* tab, const char* name);

/**
 * @brief Enter a new scope level.
 * @param tab Pointer to symbol table.
 *
 * Increments current_scope level.
 * Called when entering a new block (function, compound statement, etc.).
 */
void symtab_enter_scope(symtab_t* tab);

/**
 * @brief Exit the current scope level.
 * @param tab Pointer to symbol table.
 *
 * Removes all symbols at the current scope level.
 * Restores stack offset to value before scope was entered.
 * Decrements current_scope level.
 */
void symtab_exit_scope(symtab_t* tab);

/**
 * @brief Get the size of a type in bytes.
 * @param type Symbol type.
 * @return Size in bytes (4 for int/ptr, 1 for char, 0 for void).
 */
int sym_type_size(sym_type_t type);

#endif // SYMTAB_H
