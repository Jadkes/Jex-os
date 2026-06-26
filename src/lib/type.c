/**
 * @file type.c
 * @brief Type system implementation for TCC compiler.
 *
 * Implement type creation, size calculation, and type checking.
 * Simple factory functions for each type kind.
 * Thread-safety: Single-threaded compiler, no concurrency needed.
 */

#include "type.h"
#include <string.h>

/**
 * type_create - Create a basic type (int, char, void).
 *
 * @param kind Type kind (TYPE_INT, TYPE_CHAR, TYPE_VOID).
 * @return Initialized type_t struct.
 */
type_t type_create(type_kind_t kind) {
    type_t t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;

    switch (kind) {
        case TYPE_VOID:
            t.size = 0;
            break;
        case TYPE_INT:
            t.size = 4;
            break;
        case TYPE_CHAR:
            t.size = 1;
            break;
        case TYPE_PTR:
            t.size = 4;
            break;
        case TYPE_FUNC:
            t.size = 0;
            break;
        case TYPE_ARRAY:
            t.size = 0;
            break;
    }

    return t;
}

/**
 * type_create_void - Create a void type.
 *
 * @return Void type with size=0.
 */
type_t type_create_void(void) {
    return type_create(TYPE_VOID);
}

/**
 * type_create_ptr - Create a pointer type.
 * @param base Base type that pointer points to.
 * @return Pointer type.
 */
type_t type_create_ptr(type_t* base) {
    type_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TYPE_PTR;
    t.size = 4;
    t.base = base;
    return t;
}

/**
 * type_size - Get the size of a type in bytes.
 * @param type Type to query.
 * @return Size in bytes.
 */
uint32_t type_size(type_t* type) {
    if (!type) return 0;

    switch (type->kind) {
        case TYPE_VOID:
            return 0;
        case TYPE_INT:
            return 4;
        case TYPE_CHAR:
            return 1;
        case TYPE_PTR:
            return 4;
        case TYPE_ARRAY:
            if (type->base) {
                return type_size(type->base) * type->array_len;
            }
            return 0;
        case TYPE_FUNC:
            return 0;
        default:
            return 0;
    }
}

/**
 * type_compatible - Check if two types are compatible.
 * @param a First type.
 * @param b Second type.
 * @return 1 if compatible, 0 otherwise.
 */
int type_compatible(type_t* a, type_t* b) {
    if (!a || !b) return 0;

    if (a->kind == b->kind) {
        if (a->kind == TYPE_PTR) {
            if (!a->base || !b->base) return 0;
            return type_compatible(a->base, b->base);
        }
        return 1;
    }

    if (a->kind == TYPE_VOID || b->kind == TYPE_VOID) {
        return 0;
    }

    return 0;
}

/**
 * type_is_arithmetic - Check if a type is arithmetic (int or char).
 * @param type Type to check.
 * @return 1 if arithmetic, 0 otherwise.
 */
int type_is_arithmetic(type_t* type) {
    if (!type) return 0;
    return (type->kind == TYPE_INT || type->kind == TYPE_CHAR);
}

/**
 * type_is_pointer - Check if a type is a pointer.
 * @param type Type to check.
 * @return 1 if pointer, 0 otherwise.
 */
int type_is_pointer(type_t* type) {
    if (!type) return 0;
    return (type->kind == TYPE_PTR);
}
