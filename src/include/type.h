/**
 * @file type.h
 * @brief Type system for TCC compiler.
 *
 * Purpose: Define type representations for the compiler.
 * Design: Simple type enum with size information.
 * Thread-safety: Single-threaded compiler, no concurrency needed.
 */

#ifndef TYPE_H
#define TYPE_H

#include <stdint.h>

/**
 * @brief Basic type kinds supported by TCC.
 */
typedef enum {
    TYPE_VOID,        /**< void type */
    TYPE_INT,          /**< int type (32-bit) */
    TYPE_CHAR,         /**< char type (8-bit) */
    TYPE_PTR,          /**< pointer type */
    TYPE_FUNC,         /**< function type */
    TYPE_ARRAY,        /**< array type */
} type_kind_t;

/**
 * @brief Type structure with kind and additional info.
 *
 * WHY: Need to track type kind and size for code generation.
 *       Pointers need base type, arrays need element count.
 */
typedef struct type_t {
    type_kind_t kind;          /**< Type kind */
    uint32_t size;             /**< Size in bytes */
    struct type_t* base;      /**< For pointers: base type; for arrays: element type */
    uint32_t array_len;        /**< For arrays: number of elements */
} type_t;

/**
 * @brief Create a basic type (int, char, void).
 *
 * WHY: Factory function to create simple types with correct size.
 *
 * @param kind Type kind (TYPE_INT, TYPE_CHAR, TYPE_VOID).
 * @return Initialized type_t struct.
 */
type_t type_create(type_kind_t kind);

/**
 * @brief Create a pointer type.
 *
 * WHY: Pointers have same size (4 bytes in 32-bit), but need base type for deref.
 *
 * @param base Base type that pointer points to.
 * @return Pointer type.
 */
type_t type_create_ptr(type_t* base);

/**
 * @brief Get the size of a type in bytes.
 *
 * WHY: Used for stack allocation and sizeof operator.
 *
 * @param type Type to query.
 * @return Size in bytes.
 */
uint32_t type_size(type_t* type);

/**
 * @brief Check if two types are compatible.
 *
 * WHY: Type checking for assignments and comparisons.
 *
 * @param a First type.
 * @param b Second type.
 * @return 1 if compatible, 0 otherwise.
 */
int type_compatible(type_t* a, type_t* b);

/**
 * @brief Check if a type is arithmetic (int or char).
 *
 * WHY: Used to determine if operators can be applied.
 *
 * @param type Type to check.
 * @return 1 if arithmetic, 0 otherwise.
 */
int type_is_arithmetic(type_t* type);

/**
 * @brief Check if a type is a pointer.
 *
 * WHY: Used for pointer arithmetic and dereference.
 *
 * @param type Type to check.
 * @return 1 if pointer, 0 otherwise.
 */
int type_is_pointer(type_t* type);

#endif // TYPE_H
