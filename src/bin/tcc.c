/**
 * @file tcc.c
 * @brief Tiny C Compiler (TCC) - In-kernel subset.
 *
 * Implements a very simplified C compiler that can tokenize source code,
 * generate x86 machine code, and wrap it in an ELF32 executable.
 * Used for the JexOS self-hosting demonstration.
 */

#include "tcc.h"
#include "kheap.h"
#include "string.h"
#include "elf.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void log_serial(const char* str);

/**
 * @brief Convert integer to string.
 */
static void int_to_string(int n, char* str) {
    int i = 0; int is_neg = 0;
    if (n == 0) { str[0] = '0'; str[1] = '\0'; return; }
    if (n < 0) { is_neg = 1; n = -n; }
    while (n > 0) { str[i++] = n % 10 + '0'; n /= 10; }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    for (int j = 0; j < i/2; j++) {
        char temp = str[j]; str[j] = str[i-1-j]; str[i-1-j] = temp;
    }
}

/**
 * @brief C Keywords lookup table.
 */
static const struct {
    const char* name;
    token_type_t type;
} keywords[] = {
    {"int", TOK_INT},
    {"char", TOK_CHAR},
    {"void", TOK_VOID},
    {"return", TOK_RETURN},
    {"if", TOK_IF},
    {"else", TOK_ELSE},
    {"while", TOK_WHILE},
    {"for", TOK_FOR},
    {NULL, TOK_ERROR}
};

/**
 * @brief Create a new compiler state.
 */
tcc_state_t* tcc_new(void) {
    tcc_state_t* s = (tcc_state_t*)kmalloc(sizeof(tcc_state_t));
    if (!s) return NULL;
    
    s->source_code = NULL;
    s->output_buffer = NULL;
    s->output_size = 0;
    s->position = 0;
    s->error_count = 0;
    
    return s;
}

/**
 * @brief Clean up compiler state and associated buffers.
 */
void tcc_delete(tcc_state_t* s) {
    if (s) {
        if (s->source_code) kfree(s->source_code);
        if (s->output_buffer) kfree(s->output_buffer);
        kfree(s);
    }
}

/**
 * @brief Check if an identifier is a reserved C keyword.
 */
static token_type_t lookup_keyword(const char* str) {
    for (int i = 0; keywords[i].name; i++) {
        if (strcmp(str, keywords[i].name) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/**
 * @brief Lexical Analyzer: Convert C source into a stream of tokens.
 */
int tokenize_c_code(const char* source, token_t* tokens, int max_tokens) {
    int pos = 0;
    int token_count = 0;
    
    while (source[pos] != '\0' && token_count < max_tokens) {
        while (is_whitespace(source[pos])) pos++;
        if (source[pos] == '\0') break;
        
        token_t* tok = &tokens[token_count];
        
        switch (source[pos]) {
            case '+': tok->type = TOK_PLUS; pos++; break;
            case '-': tok->type = TOK_MINUS; pos++; break;
            case '*': tok->type = TOK_STAR; pos++; break;
            case '/': 
                if (source[pos+1] == '/') { /* Skip single-line comments */
                    while (source[pos] && source[pos] != '\n') pos++;
                    continue;
                } else {
                    tok->type = TOK_SLASH; pos++; 
                }
                break;
            case '=': 
                if (source[pos+1] == '=') { tok->type = TOK_EQ; pos += 2; }
                else { tok->type = TOK_ASSIGN; pos++; }
                break;
            case '!':
                if (source[pos+1] == '=') { tok->type = TOK_NEQ; pos += 2; }
                else { tok->type = TOK_ERROR; pos++; }
                break;
            case '<':
                if (source[pos+1] == '=') { tok->type = TOK_LE; pos += 2; }
                else { tok->type = TOK_LT; pos++; }
                break;
            case '>':
                if (source[pos+1] == '=') { tok->type = TOK_GE; pos += 2; }
                else { tok->type = TOK_GT; pos++; }
                break;
            case ';': tok->type = TOK_SEMICOLON; pos++; break;
            case ',': tok->type = TOK_COMMA; pos++; break;
            case '(': tok->type = TOK_LPAREN; pos++; break;
            case ')': tok->type = TOK_RPAREN; pos++; break;
            case '{': tok->type = TOK_LBRACE; pos++; break;
            case '}': tok->type = TOK_RBRACE; pos++; break;
            case '[': tok->type = TOK_LBRACKET; pos++; break;
            case ']': tok->type = TOK_RBRACKET; pos++; break;
            
            default:
                if (is_digit(source[pos])) {
                    int val = 0;
                    while (is_digit(source[pos])) {
                        val = val * 10 + (source[pos] - '0');
                        pos++;
                    }
                    tok->type = TOK_NUMBER;
                    tok->int_val = val;
                }
                else if (source[pos] == '"') {
                    pos++; /* Skip opening quote */
                    char str_val[256];
                    int i = 0;
                    while (source[pos] != '"' && source[pos] != '\0' && i < 255) {
                        if (source[pos] == '\\' && source[pos+1] != '\0') {
                            pos++;
                            if (source[pos] == 'n') str_val[i++] = '\n';
                            else if (source[pos] == 't') str_val[i++] = '\t';
                            else if (source[pos] == 'r') str_val[i++] = '\r';
                            else if (source[pos] == '"') str_val[i++] = '"';
                            else if (source[pos] == '\\') str_val[i++] = '\\';
                            else str_val[i++] = source[pos];
                            pos++;
                        } else {
                            str_val[i++] = source[pos++];
                        }
                    }
                    str_val[i] = '\0';
                    if (source[pos] == '"') pos++;
                    
                    tok->type = TOK_STRING;
                    tok->str = (char*)kmalloc(strlen(str_val) + 1);
                    strcpy(tok->str, str_val);
                }
                else if (is_alpha(source[pos])) {
                    char ident[64];
                    int i = 0;
                    while (is_alpha(source[pos]) || is_digit(source[pos])) {
                        if (i < 63) ident[i++] = source[pos];
                        pos++;
                    }
                    ident[i] = '\0';
                    
                    tok->type = lookup_keyword(ident);
                    if (tok->type == TOK_IDENT) {
                        tok->str = (char*)kmalloc(strlen(ident) + 1);
                        strcpy(tok->str, ident);
                    }
                } else {
                    tok->type = TOK_ERROR;
                    pos++;
                }
        }
        
        if (tok->type != TOK_ERROR) token_count++;
    }
    
    if (token_count < max_tokens) {
        tokens[token_count].type = TOK_EOF;
        token_count++;
    }
    
    return token_count;
}

/**
 * @brief Emit 'mov eax, imm32' instruction.
 */
static void emit_mov_eax_imm(uint8_t* buf, uint32_t* pos, uint32_t imm) {
    buf[(*pos)++] = 0xB8;
    buf[(*pos)++] = imm & 0xFF;
    buf[(*pos)++] = (imm >> 8) & 0xFF;
    buf[(*pos)++] = (imm >> 16) & 0xFF;
    buf[(*pos)++] = (imm >> 24) & 0xFF;
}

/**
 * @brief Simple Parser and Code Generator.
 * 
 * Maps basic C constructs (print, malloc, return) to x86 machine code
 * and syscalls.
 */
int parse_c_tokens(token_t* tokens, uint8_t* output, uint32_t* size) {
    uint32_t pos = 0;
    int i = 0;
    uint8_t string_table[1024];
    uint32_t string_pos = 0;
    uint32_t base_addr = 0x08048080; /* Standard ELF load address */
    
    while (tokens[i].type != TOK_EOF && pos < (*size - 200)) {
        /* Handle print("...") / printf("...") */
        if (tokens[i].type == TOK_IDENT && 
            (strcmp(tokens[i].str, "print") == 0 || strcmp(tokens[i].str, "printf") == 0) &&
            tokens[i+1].type == TOK_LPAREN &&
            tokens[i+2].type == TOK_STRING &&
            tokens[i+3].type == TOK_RPAREN &&
            tokens[i+4].type == TOK_SEMICOLON) {
            
            uint32_t str_offset = string_pos;
            int len = strlen(tokens[i+2].str) + 1;
            memcpy(string_table + string_pos, tokens[i+2].str, len);
            string_pos += len;
            
            /* mov eax, 0 (SYS_PRINT) */
            emit_mov_eax_imm(output, &pos, 0);
            
            /* mov ebx, str_addr (temp offset stored) */
            output[pos++] = 0xBB;
            uint32_t* patch_ptr = (uint32_t*)(output + pos);
            *patch_ptr = str_offset;
            pos += 4;
            
            /* int 0x80 */
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 5;
        }
        /* Handle malloc(size) */
        else if (tokens[i].type == TOK_IDENT && strcmp(tokens[i].str, "malloc") == 0 &&
                 tokens[i+1].type == TOK_LPAREN &&
                 tokens[i+2].type == TOK_NUMBER &&
                 tokens[i+3].type == TOK_RPAREN) {
            
            /* mov ebx, size */
            output[pos++] = 0xBB;
            uint32_t val = tokens[i+2].int_val;
            *(uint32_t*)(output + pos) = val;
            pos += 4;
            
            /* mov eax, 7 (SYS_SBRK) */
            emit_mov_eax_imm(output, &pos, 7);
            
            /* int 0x80 */
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 4;
        }
        /* Handle return number; */
        else if (tokens[i].type == TOK_RETURN && 
            tokens[i+1].type == TOK_NUMBER && 
            tokens[i+2].type == TOK_SEMICOLON) {
            
            /* mov ebx, number */
            output[pos++] = 0xBB;
            *(uint32_t*)(output + pos) = tokens[i+1].int_val;
            pos += 4;
            
            /* mov eax, 1 (SYS_EXIT) */
            emit_mov_eax_imm(output, &pos, 1);
            
            /* int 0x80 */
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 3;
        }
        /* Handle inline assembly: asm("...") */
        else if (tokens[i].type == TOK_IDENT && strcmp(tokens[i].str, "asm") == 0) {
            int j = i + 1;
            while (tokens[j].type != TOK_STRING && tokens[j].type != TOK_EOF) j++;
            if (tokens[j].type == TOK_STRING) {
                if (strstr(tokens[j].str, "int $0x80")) {
                     output[pos++] = 0xCD; output[pos++] = 0x80;
                }
            }
            while (tokens[i].type != TOK_SEMICOLON && tokens[i].type != TOK_EOF) i++;
            if (tokens[i].type == TOK_SEMICOLON) i++;
        }
        else i++;
    }
    
    /* Append string table to the end of the code segment */
    uint32_t code_end_offset = pos;
    memcpy(output + pos, string_table, string_pos);
    pos += string_pos;
    
    /* Patch string offsets into absolute virtual addresses */
    for (uint32_t p = 0; p < code_end_offset; p++) {
        if (output[p] == 0xBB) {
             uint32_t offset = *(uint32_t*)(output + p + 1);
             if (offset < string_pos) {
                 *(uint32_t*)(output + p + 1) = base_addr + code_end_offset + offset;
             }
        }
    }
    
    *size = pos;
    return pos > 0 ? 0 : -1;
}

/**
 * @brief Wrap machine code into a valid ELF32 executable.
 */
int generate_elf32(uint8_t* code, uint32_t code_size, uint8_t** elf_output, uint32_t* elf_size) {
    #define ELF_HEADER_SIZE 52
    #define PROGRAM_HEADER_SIZE 32
    
    *elf_size = ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE + code_size;
    *elf_output = (uint8_t*)kmalloc(*elf_size);
    if (!*elf_output) return -1;
    
    uint8_t* buf = *elf_output;
    uint32_t pos = 0;
    
    /* ELF Header */
    memcpy(buf + pos, "\x7F" "ELF" "\x01\x01\x01\x00", 8); pos += 8;
    memset(buf + pos, 0, 8); pos += 8;
    *(uint16_t*)(buf + pos) = 2; pos += 2; /* ET_EXEC */
    *(uint16_t*)(buf + pos) = 3; pos += 2; /* EM_386 */
    *(uint32_t*)(buf + pos) = 1; pos += 4; /* EV_CURRENT */
    *(uint32_t*)(buf + pos) = 0x08048080; pos += 4; /* Entry point */
    *(uint32_t*)(buf + pos) = 52; pos += 4; /* Program header offset */
    *(uint32_t*)(buf + pos) = 0; pos += 4;  /* Section header offset */
    *(uint32_t*)(buf + pos) = 0; pos += 4;  /* Flags */
    *(uint16_t*)(buf + pos) = 52; pos += 2; /* ELF header size */
    *(uint16_t*)(buf + pos) = 32; pos += 2; /* Program header size */
    *(uint16_t*)(buf + pos) = 1; pos += 2;  /* Phnum */
    *(uint16_t*)(buf + pos) = 0; pos += 2;
    *(uint16_t*)(buf + pos) = 0; pos += 2;
    *(uint16_t*)(buf + pos) = 0; pos += 2;
    
    /* Program Header */
    *(uint32_t*)(buf + pos) = 1; pos += 4; /* PT_LOAD */
    *(uint32_t*)(buf + pos) = 52 + 32; pos += 4; /* Offset */
    *(uint32_t*)(buf + pos) = 0x08048080; pos += 4; /* Vaddr */
    *(uint32_t*)(buf + pos) = 0x08048080; pos += 4; /* Paddr */
    *(uint32_t*)(buf + pos) = code_size; pos += 4; /* Filesz */
    *(uint32_t*)(buf + pos) = code_size; pos += 4; /* Memsz */
    *(uint32_t*)(buf + pos) = 7; pos += 4; /* RWX */
    *(uint32_t*)(buf + pos) = 4096; pos += 4; /* Align */
    
    /* Append Code Segment */
    memcpy(buf + pos, code, code_size);
    
    return 0;
}

/**
 * @brief Main compiler interface: Source string -> ELF in memory.
 */
int tcc_compile_string(tcc_state_t* s, const char* str) {
    if (!s || !str) return -1;
    s->source_code = (char*)kmalloc(strlen(str) + 1);
    strcpy(s->source_code, str);
    
    token_t* tokens = (token_t*)kmalloc(sizeof(token_t) * 1024);
    int token_count = tokenize_c_code(str, tokens, 1024);
    if (token_count == 0) { kfree(tokens); return -1; }
    
    uint8_t code_buffer[4096];
    uint32_t code_size = sizeof(code_buffer);
    if (parse_c_tokens(tokens, code_buffer, &code_size) < 0) { kfree(tokens); return -1; }
    kfree(tokens);

    uint8_t* elf_output;
    uint32_t elf_size;
    if (generate_elf32(code_buffer, code_size, &elf_output, &elf_size) < 0) return -1;
    
    if (s->output_buffer) kfree(s->output_buffer);
    s->output_buffer = (char*)elf_output;
    s->output_size = elf_size;
    
    return 0;
}

/**
 * @brief Get the generated ELF binary from the compiler state.
 */
int tcc_output_memory(tcc_state_t* s, uint8_t** output, uint32_t* size) {
    if (!s || !output || !size || !s->output_buffer) return -1;
    *output = (uint8_t*)s->output_buffer;
    *size = s->output_size;
    return 0;
}

int tcc_output_file(tcc_state_t* s, const char* filename) {
    (void)s; (void)filename; return 0;
}

void tcc_set_error_func(tcc_state_t* s, void* error_opaque, void (*error_func)(void*, const char*)) {
    (void)s; (void)error_opaque; (void)error_func;
}
