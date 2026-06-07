/**
 * @file type.c
 * @brief Type system implementation for TCC compiler.
 *
 * Purpose: Define type representations and operations for the compiler.
 * Design: Simple type struct with kind and size information.
 * Thread-safety: Single-threaded compiler, no concurrency needed.
 */

#include "type.h"
#include <string.h>

/**
 * @brief Create a basic type (int, char, void).
 *
 * WHY: Factory function to create simple types with correct size.
 *
 * @param kind Type kind (TYPE_INT, TYPE_CHAR, TYPE_VOID).
 * @return Initialized type_t struct.
 */
type_t type_create(type_kind_t kind) {
    type_t type;
    memset(&type, 0, sizeof(type_t));

    type.kind = kind;

    switch (kind) {
        case TYPE_INT:
            type.size = 4;  /* 32-bit */
            break;
        case TYPE_CHAR:
            type.size = 1;  /* 8-bit */
            break;
        case TYPE_VOID:
            type.size = 0;  /* No size */
            break;
        case TYPE_PTR:
            type.size = 4;  /* 32-bit pointer */
            break;
        default:
            type.size = 0;
            break;
    }

    type.base = NULL;
    type.array_len = 0;

    return type;
}

/**
 * @brief Create a pointer type.
 *
 * WHY: Pointers have same size (4 bytes in 32-bit), but need base type for deref.
 *
 * @param base Base type that pointer points to.
 * @return Pointer type.
 */
type_t type_create_ptr(type_t* base) {
    type_t type;
    memset(&type, 0, sizeof(type_t));

    type.kind = TYPE_PTR;
    type.size = 4;  /* 32-bit pointer */
    type.base = (struct type_t*)base;
    type.array_len = 0;

    return type;
}

/**
 * @brief Get the size of a type in bytes.
 *
 * WHY: Used for stack allocation and sizeof operator.
 *
 * @param type Type to query.
 * @return Size in bytes.
 */
uint32_t type_size(type_t* type) {
    if (!type) return 0;

    /* For pointers, always 4 bytes */
    if (type->kind == TYPE_PTR) {
        return 4;
    }

    /* For arrays, size = element size * length */
    if (type->kind == TYPE_ARRAY) {
        if (type->base) {
            return type_size(type->base) * type->array_len;
        }
        return 0;
    }

    return type->size;
}

/**
 * @brief Check if two types are compatible.
 *
 * WHY: Type checking for assignments and comparisons.
 *
 * @param a First type.
 * @param b Second type.
 * @return 1 if compatible,0 otherwise.
 */
int type_compatible(type_t* a, type_t* b) {
    if (!a || !b) return 0;

    /* Same kind = compatible */
    if (a->kind == b->kind) {
        /* For pointers, check base type */
        if (a->kind == TYPE_PTR) {
            if (!a->base || !b->base) return 0;
            return type_compatible(a->base, b->base);
        }
        return 1;
    }

    /* int and char are compatible (both arithmetic) */
    if ((a->kind == TYPE_INT && b->kind == TYPE_CHAR) ||
        (a->kind == TYPE_CHAR && b->kind == TYPE_INT)) {
        return 1;
    }

    /* Pointer to void is compatible with any pointer */
    if (a->kind == TYPE_PTR && b->kind == TYPE_PTR) {
        if (a->base && a->base->kind == TYPE_VOID) return 1;
        if (b->base && b->base->kind == TYPE_VOID) return 1;
    }

    return 0;
}

/**
 * @brief Check if a type is arithmetic (int or char).
 *
 * WHY: Used to determine if operators can be applied.
 *
 * @param type Type to check.
 * @return 1 if arithmetic, 0 otherwise.
 */
int type_is_arithmetic(type_t* type) {
    if (!type) return 0;

    return (type->kind == TYPE_INT || type->kind == TYPE_CHAR);
}

/**
 * @brief Check if a type is a pointer.
 *
 * WHY: Used for pointer arithmetic and dereference.
 *
 * @param type Type to check.
 * @return 1 if pointer, 0 otherwise.
 */
int type_is_pointer(type_t* type) {
    if (!type) return 0;

    return (type->kind == TYPE_PTR);
}
