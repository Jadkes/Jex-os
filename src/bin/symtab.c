/**
 * @file symtab.c
 * @brief Symbol table implementation for Tiny C Compiler (TCC).
 *
 * Fixed-size array implementation with scope management.
 * No dynamic allocation - uses SYM_TABLE_SIZE entries.
 */

#include "symtab.h"
#include "string.h"

/**
 * @brief Initialize the symbol table.
 */
void symtab_init(symtab_t* tab) {
    if (!tab) return;

    /* Clear all entries */
    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        tab->entries[i].name[0] = '\0';
        tab->entries[i].type = SYM_UNDEF;
        tab->entries[i].offset = 0;
        tab->entries[i].scope_level = -1; /* -1 means empty */
    }

    tab->current_scope = 0;
    tab->current_offset = 0;
    tab->next_free = 0;
}

/**
 * @brief Find first free slot in the table.
 * @return Index of free slot, or -1 if table is full.
 */
static int find_free_slot(symtab_t* tab) {
    /* Start from next_free hint, wrap around */
    int start = tab->next_free;

    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        int idx = (start + i) % SYM_TABLE_SIZE;
        if (tab->entries[idx].scope_level == -1) {
            tab->next_free = (idx + 1) % SYM_TABLE_SIZE;
            return idx;
        }
    }

    return -1; /* Table full */
}

/**
 * @brief Add a symbol to the table.
 */
int symtab_add(symtab_t* tab, const char* name, sym_type_t type, int offset, int scope_level) {
    if (!tab || !name || scope_level <0) return -1;

    /* Check if name already exists at same scope (redefinition) */
    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        if (tab->entries[i].scope_level == scope_level &&
            strcmp(tab->entries[i].name, name) == 0) {
            return -1; /* Symbol already defined at this scope */
        }
    }

    int idx = find_free_slot(tab);
    if (idx == -1) return -1; /* Table full */

    /* Copy name (max SYM_NAME_MAX-1 chars) */
    strncpy(tab->entries[idx].name, name, SYM_NAME_MAX - 1);
    tab->entries[idx].name[SYM_NAME_MAX - 1] = '\0';

    tab->entries[idx].type = type;
    tab->entries[idx].base_type = SYM_INT; /* default base type */
    tab->entries[idx].offset = offset;
    tab->entries[idx].scope_level = scope_level;
    tab->entries[idx].array_size = 0;

    return 0;
}

int symtab_add_array(symtab_t* tab, const char* name, sym_type_t type, int offset, int scope_level, int array_size) {
    int result = symtab_add(tab, name, SYM_ARRAY, offset, scope_level);
    if (result < 0) return result;

    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        if (tab->entries[i].scope_level == scope_level &&
            strcmp(tab->entries[i].name, name) == 0) {
            tab->entries[i].array_size = array_size;
            break;
        }
    }

    return 0;
}

/**
 * @brief Look up a symbol by name.
 *
 * Searches from newest to oldest (reverse order).
 * Returns most recently added symbol with matching name.
 */
symbol_t* symtab_lookup(symtab_t* tab, const char* name) {
    if (!tab || !name) return NULL;

    /* Search in reverse order to find most recent definition */
    for (int i = SYM_TABLE_SIZE - 1; i >= 0; i--) {
        if (tab->entries[i].scope_level != -1 &&
            strcmp(tab->entries[i].name, name) == 0) {
            return &tab->entries[i];
        }
    }

    return NULL;
}

/**
 * @brief Enter a new scope level.
 */
void symtab_enter_scope(symtab_t* tab) {
    if (!tab) return;

    tab->current_scope++;
}

/**
 * @brief Exit the current scope level.
 *
 * Removes all symbols at current scope.
 * Restores stack offset to value before scope was entered.
 * Note: In this simple implementation, we don't track previous offset.
 * Caller should save/restore offset manually if needed.
 */
void symtab_exit_scope(symtab_t* tab) {
    if (!tab || tab->current_scope <= 0) return;

    for (int i = 0; i < SYM_TABLE_SIZE; i++) {
        if (tab->entries[i].scope_level == tab->current_scope) {
            tab->entries[i].name[0] = '\0';
            tab->entries[i].type = SYM_UNDEF;
            tab->entries[i].offset = 0;
            tab->entries[i].scope_level = -1;
            tab->entries[i].array_size = 0;
        }
    }

    tab->current_scope--;
}

/**
 * @brief Get the size of a type in bytes.
 */
int sym_type_size(sym_type_t type) {
    switch (type) {
        case SYM_INT:
        case SYM_PTR:
            return 4; /* 32-bit */
        case SYM_CHAR:
            return 1;
        case SYM_VOID:
        case SYM_FUNC:
            return 0; /* No size or special handling */
        case SYM_ARRAY:
            return 0;
        default:
            return 0;
    }
}
