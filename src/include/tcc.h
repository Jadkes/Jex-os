/**
 * @file tcc.h
 * @brief Tiny C Compiler (TCC) - subset interface for JexOS.
 */

#ifndef TCC_H
#define TCC_H

#include <stdint.h>

/**
 * @struct tcc_state_t
 * @brief Holds the internal state of a compilation process.
 */
typedef struct {
    char* source_code;      /**< Pointer to the source string. */
    char* output_buffer;    /**< Pointer to the generated ELF image. */
    uint32_t output_size;   /**< Size of the generated ELF. */
    uint32_t position;      /**< Current parsing position. */
    int error_count;        /**< Number of compilation errors encountered. */
} tcc_state_t;

/* TCC API functions */

/**
 * @brief Create a new compiler instance.
 */
tcc_state_t* tcc_new(void);

/**
 * @brief Delete a compiler instance and free its memory.
 */
void tcc_delete(tcc_state_t* s);

/**
 * @brief Compile a C source string into an in-memory ELF executable.
 * @return 0 on success, -1 on failure.
 */
int tcc_compile_string(tcc_state_t* s, const char* str);

/**
 * @brief Retrieve the generated binary data from memory.
 */
int tcc_output_memory(tcc_state_t* s, uint8_t** output, uint32_t* size);

/**
 * @brief Token types for the lexical analyzer.
 */
typedef enum {
    TOK_INT, TOK_CHAR, TOK_VOID, TOK_RETURN, TOK_IF, TOK_ELSE,
    TOK_WHILE, TOK_FOR, TOK_IDENT, TOK_NUMBER, TOK_STRING,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_ASSIGN,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_SEMICOLON, TOK_COMMA, TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_EOF, TOK_ERROR
} token_type_t;

/**
 * @struct token_t
 * @brief Represents a single C language token.
 */
typedef struct {
    token_type_t type;
    char* str;
    int int_val;
} token_t;

#endif // TCC_H
