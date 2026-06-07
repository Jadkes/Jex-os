/**
 * @file expr.h
 * @brief Expression evaluator for TCC compiler using precedence climbing.
 */

#ifndef EXPR_H
#define EXPR_H

#include <stdint.h>
#include "tcc.h"
#include "symtab.h"

/* Precedence levels (higher = tighter binding) */
#define PREC_NONE      0   /* below all operators */
#define PREC_ASSIGN    1   /* = (not implemented yet) */
#define PREC_LOGOR     2   /* || */
#define PREC_LOGAND    3   /* && */
#define PREC_EQ        4   /* == != */
#define PREC_REL       5   /* < > <= >= */
#define PREC_ADD       6   /* + - */
#define PREC_MUL       7   /* * / % */
#define PREC_PREFIX    8   /* unary + - ! ~ & * (deref) */

/**
 * expr_parse - Parse and generate code for an expression.
 *
 * Uses precedence climbing algorithm (O(n) time).
 * Result always left in eax register.
 *
 * @param tokens Token array.
 * @param pos Pointer to current token index (updated).
 * @param tab Symbol table for variable lookup.
 * @param output Output buffer for generated x86 code.
 * @param out_pos Pointer to current output position (updated).
 * @return 0 on success, -1 on error.
 */
int expr_parse(token_t* tokens, int* pos, symtab_t* tab,
              uint8_t* output, uint32_t* out_pos);

/**
 * expr_get_precedence - Get precedence level for binary operator.
 *
 * @param type Token type.
 * @return Precedence level (0 if not binary operator).
 */
int expr_get_precedence(token_type_t type);

/**
 * expr_is_binary_op - Check if token is a binary operator.
 *
 * @param type Token type.
 * @return 1 if binary operator, 0 otherwise.
 */
int expr_is_binary_op(token_type_t type);

#endif // EXPR_H
