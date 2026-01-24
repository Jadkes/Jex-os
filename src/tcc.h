#ifndef TCC_H
#define TCC_H

#include <stdint.h>

// Simple C compiler structure for JexOS
typedef struct {
    char* source_code;
    char* output_buffer;
    uint32_t output_size;
    uint32_t position;
    int error_count;
} tcc_state_t;

// TCC API functions
tcc_state_t* tcc_new(void);
void tcc_delete(tcc_state_t* s);
int tcc_compile_string(tcc_state_t* s, const char* str);
int tcc_output_file(tcc_state_t* s, const char* filename);
int tcc_output_memory(tcc_state_t* s, uint8_t** output, uint32_t* size);
void tcc_set_error_func(tcc_state_t* s, void* error_opaque, void (*error_func)(void*, const char*));

// Simple C tokenizer
typedef enum {
    TOK_INT,
    TOK_CHAR,
    TOK_VOID,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_ASSIGN,
    TOK_EQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_SEMICOLON,
    TOK_COMMA,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_EOF,
    TOK_ERROR
} token_type_t;

typedef struct {
    token_type_t type;
    char* str;
    int int_val;
} token_t;

// Compiler functions
int tokenize_c_code(const char* source, token_t* tokens, int max_tokens);
int parse_c_tokens(token_t* tokens, uint8_t* output, uint32_t* size);
int generate_elf32(uint8_t* code, uint32_t code_size, uint8_t** elf_output, uint32_t* elf_size);

#endif