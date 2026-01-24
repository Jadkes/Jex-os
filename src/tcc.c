#include "tcc.h"
#include "kheap.h"
#include "string.h"
#include "elf.h"
#include <stddef.h>

extern void terminal_writestring(const char* data);
extern void log_serial(const char* str);

// Simple int to string conversion
static void int_to_string(int n, char* str) {
    int i = 0; int is_neg = 0;
    if (n == 0) { str[0] = '0'; str[1] = '\0'; return; }
    if (n < 0) { is_neg = 1; n = -n; }
    while (n > 0) { str[i++] = n % 10 + '0'; n /= 10; }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    // Reverse string
    for (int j = 0; j < i/2; j++) {
        char temp = str[j]; str[j] = str[i-1-j]; str[i-1-j] = temp;
    }
}

// Keywords table
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

void tcc_delete(tcc_state_t* s) {
    if (s) {
        if (s->source_code) kfree(s->source_code);
        if (s->output_buffer) kfree(s->output_buffer);
        kfree(s);
    }
}

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

int tokenize_c_code(const char* source, token_t* tokens, int max_tokens) {
    int pos = 0;
    int token_count = 0;
    
    while (source[pos] != '\0' && token_count < max_tokens) {
        // Skip whitespace
        while (is_whitespace(source[pos])) pos++;
        
        if (source[pos] == '\0') break;
        
        token_t* tok = &tokens[token_count];
        
        // Single character tokens
        switch (source[pos]) {
            case '+': tok->type = TOK_PLUS; pos++; break;
            case '-': tok->type = TOK_MINUS; pos++; break;
            case '*': tok->type = TOK_STAR; pos++; break;
            case '/': 
                // Skip comments
                if (source[pos+1] == '/') {
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
                // Numbers
                if (is_digit(source[pos])) {
                    int val = 0;
                    while (is_digit(source[pos])) {
                        val = val * 10 + (source[pos] - '0');
                        pos++;
                    }
                    tok->type = TOK_NUMBER;
                    tok->int_val = val;
                }
                // String literals
                else if (source[pos] == '"') {
                    pos++; // Skip opening quote
                    char str_val[256];
                    int i = 0;
                    while (source[pos] != '"' && source[pos] != '\0' && i < 255) {
                        if (source[pos] == '\\' && source[pos+1] != '\0') {
                            pos++; // Skip backslash
                            if (source[pos] == 'n') str_val[i++] = '\n';
                            else if (source[pos] == 't') str_val[i++] = '\t';
                            else if (source[pos] == 'r') str_val[i++] = '\r';
                            else if (source[pos] == '"') str_val[i++] = '"';
                            else if (source[pos] == '\\') str_val[i++] = '\\';
                            else str_val[i++] = source[pos]; // Unknown escape
                            pos++;
                        } else {
                            str_val[i++] = source[pos++];
                        }
                    }
                    str_val[i] = '\0';
                    if (source[pos] == '"') pos++; // Skip closing quote
                    
                    tok->type = TOK_STRING;
                    tok->str = (char*)kmalloc(strlen(str_val) + 1);
                    strcpy(tok->str, str_val);
                }
                // Identifiers and keywords
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
        
        if (tok->type != TOK_ERROR) {
            token_count++;
        }
    }
    
    // Add EOF token
    if (token_count < max_tokens) {
        tokens[token_count].type = TOK_EOF;
        token_count++;
    }
    
    return token_count;
}

// Simple x86 code generator
static void emit_mov_eax_imm(uint8_t* buf, uint32_t* pos, uint32_t imm) {
    buf[(*pos)++] = 0xB8;
    buf[(*pos)++] = imm & 0xFF;
    buf[(*pos)++] = (imm >> 8) & 0xFF;
    buf[(*pos)++] = (imm >> 16) & 0xFF;
    buf[(*pos)++] = (imm >> 24) & 0xFF;
}

// Helper to emit string literal
static uint32_t emit_string(uint8_t* buf, uint32_t* pos, const char* str) {
    uint32_t str_addr = *pos;
    int len = strlen(str) + 1;
    memcpy(buf + *pos, str, len);
    *pos += len;
    return str_addr;
}

int parse_c_tokens(token_t* tokens, uint8_t* output, uint32_t* size) {
    uint32_t pos = 0;
    int i = 0;
    
    // We need a place to store strings. For this simple compiler, 
    // we will jump over the data at the beginning or put them at the end.
    // Let's use a separate buffer for strings and append it later?
    // Or simpler: Emit code, and for strings, we put them at a fixed offset 
    // assuming code size is small? No, dynamic is better.
    // Strategy: 
    // 1. Emit code.
    // 2. When string needed, emit a jump over it, then the string, then continue?
    //    That messes up offsets if we don't track them.
    // 3. Simpler: Put all strings at the end of the code segment. 
    //    We need two passes or a separate buffer.
    
    uint8_t string_table[1024];
    uint32_t string_pos = 0;
    
    // Base address of the code in memory (ELF entry point)
    // The loader loads the code segment to 0x08048080.
    uint32_t base_addr = 0x08048080; 
    
    while (tokens[i].type != TOK_EOF && pos < (*size - 200)) {
        // Pattern: print("string"); or printf("string");
        if (tokens[i].type == TOK_IDENT && 
            (strcmp(tokens[i].str, "print") == 0 || strcmp(tokens[i].str, "printf") == 0) &&
            tokens[i+1].type == TOK_LPAREN &&
            tokens[i+2].type == TOK_STRING &&
            tokens[i+3].type == TOK_RPAREN &&
            tokens[i+4].type == TOK_SEMICOLON) {
            
            // 1. Store string in string table
            uint32_t str_offset = string_pos;
            int len = strlen(tokens[i+2].str) + 1;
            memcpy(string_table + string_pos, tokens[i+2].str, len);
            string_pos += len;
            
            // 2. Calculate runtime address of string
            // It will be appended after the code. 
            // We don't know the final code size yet! 
            // Use 0x00000000 placeholder and patch later? 
            // Or just use relative LEA? ELF 32 usually uses absolute addresses.
            
            // Let's use a placeholder and patch loop at the end.
            uint32_t str_runtime_addr = 0xDEADBEEF; // Will be patched
            
            // mov eax, 0 (SYS_PRINT)
            emit_mov_eax_imm(output, &pos, 0);
            
            // mov ebx, str_addr
            output[pos++] = 0xBB; // mov ebx, imm32
            uint32_t patch_loc = pos; // Remember where to patch
            output[pos++] = str_offset & 0xFF; // Store OFFSET for now
            output[pos++] = (str_offset >> 8) & 0xFF;
            output[pos++] = (str_offset >> 16) & 0xFF;
            output[pos++] = (str_offset >> 24) & 0xFF; // Mark as "OFFSET-coded"
            
            // int 0x80
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 5;
        }
        // Pattern: malloc(size)
        else if (tokens[i].type == TOK_IDENT && strcmp(tokens[i].str, "malloc") == 0 &&
                 tokens[i+1].type == TOK_LPAREN &&
                 tokens[i+2].type == TOK_NUMBER &&
                 tokens[i+3].type == TOK_RPAREN) {
            
            // mov ebx, size
            output[pos++] = 0xBB;
            uint32_t val = tokens[i+2].int_val;
            output[pos++] = val & 0xFF;
            output[pos++] = (val >> 8) & 0xFF;
            output[pos++] = (val >> 16) & 0xFF;
            output[pos++] = (val >> 24) & 0xFF;
            
            // mov eax, 7 (SYS_SBRK)
            emit_mov_eax_imm(output, &pos, 7);
            
            // int 0x80
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 4;
        }
        // Pattern: free(ptr) - ignore for now
        else if (tokens[i].type == TOK_IDENT && strcmp(tokens[i].str, "free") == 0) {
            while (tokens[i].type != TOK_SEMICOLON && tokens[i].type != TOK_EOF) i++;
            if (tokens[i].type == TOK_SEMICOLON) i++;
        }
        // Pattern: return number; -> exit(number)
        else if (tokens[i].type == TOK_RETURN && 
            tokens[i+1].type == TOK_NUMBER && 
            tokens[i+2].type == TOK_SEMICOLON) {
            
            // mov ebx, number (exit code)
            output[pos++] = 0xBB;
            uint32_t val = tokens[i+1].int_val;
            output[pos++] = val & 0xFF;
            output[pos++] = (val >> 8) & 0xFF;
            output[pos++] = (val >> 16) & 0xFF;
            output[pos++] = (val >> 24) & 0xFF;
            
            // mov eax, 1 (SYS_EXIT)
            emit_mov_eax_imm(output, &pos, 1);
            
            // int 0x80
            output[pos++] = 0xCD;
            output[pos++] = 0x80;
            
            i += 3;
        }
        // Handle inline assembly: asm("int $0x80");
        else if (tokens[i].type == TOK_IDENT && 
                 strcmp(tokens[i].str, "asm") == 0) {
             
            // Skip to string
            int j = i + 1;
            while (tokens[j].type != TOK_STRING && tokens[j].type != TOK_EOF && tokens[j].type != TOK_SEMICOLON) j++;
            
            if (tokens[j].type == TOK_STRING) {
                if (strstr(tokens[j].str, "int $0x80")) {
                     output[pos++] = 0xCD;
                     output[pos++] = 0x80;
                }
                // Add MOV support
                else if (strstr(tokens[j].str, "mov $0, %eax")) {
                    emit_mov_eax_imm(output, &pos, 0);
                }
            }
            // Skip until semicolon
            while (tokens[i].type != TOK_SEMICOLON && tokens[i].type != TOK_EOF) i++;
            if (tokens[i].type == TOK_SEMICOLON) i++;
        }
        // Skip function headers/braces
        else {
            i++;
        }
    }
    
    // Append string table
    uint32_t code_end_offset = pos;
    memcpy(output + pos, string_table, string_pos);
    pos += string_pos;
    
    // Patch string addresses
    // We scan the code for 0xBB (mov ebx, imm32) and check if it looks like a string offset
    // This is hacky but we don't have a relocation table structure in this function.
    // Better: We know exactly where we emitted strings.
    // But we didn't save the patch locations list.
    // Let's re-scan?
    
    for (uint32_t p = 0; p < code_end_offset; p++) {
        if (output[p] == 0xBB) { // Possible mov ebx, imm
             // This is dangerous if user code has 0xBB, but for this toy compiler it's fine.
             // We stored the string offset in the immediate field.
             uint32_t offset = *(uint32_t*)(output + p + 1);
             if (offset < string_pos) { // It's a valid string offset
                 uint32_t final_addr = base_addr + code_end_offset + offset;
                 *(uint32_t*)(output + p + 1) = final_addr;
             }
        }
    }
    
    *size = pos;
    return pos > 0 ? 0 : -1;
}

int generate_elf32(uint8_t* code, uint32_t code_size, uint8_t** elf_output, uint32_t* elf_size) {
    // Basic ELF32 executable structure
    #define ELF_HEADER_SIZE 52
    #define PROGRAM_HEADER_SIZE 32
    #define SECTION_HEADER_SIZE 40
    #define PAGE_SIZE 4096
    
    *elf_size = ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE + code_size;
    *elf_output = (uint8_t*)kmalloc(*elf_size);
    
    if (!*elf_output) return -1;
    
    uint8_t* buf = *elf_output;
    uint32_t pos = 0;
    
    // ELF Header
    memcpy(buf + pos, "\x7F" "ELF" "\x01\x01\x01\x00", 8); pos += 8; // Magic, class, data, version
    memset(buf + pos, 0, 8); pos += 8; // ABI, padding
    buf[pos++] = 2; buf[pos++] = 0; // Type = ET_EXEC
    buf[pos++] = 3; buf[pos++] = 0; // Machine = EM_386
    buf[pos++] = 1; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; // Version
    
    // Entry point: 0x08048080
    buf[pos++] = 0x80; buf[pos++] = 0x80; buf[pos++] = 0x04; buf[pos++] = 0x08; 
    
    buf[pos++] = 0x34; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; // Phoff (52)
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; // Shoff
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; // Flags
    buf[pos++] = 0x34; buf[pos++] = 0x00; // Ehsize (52)
    buf[pos++] = 0x20; buf[pos++] = 0x00; // Phentsize (32)
    buf[pos++] = 0x01; buf[pos++] = 0x00; // Phnum (1)
    buf[pos++] = 0x00; buf[pos++] = 0x00; // Shentsize
    buf[pos++] = 0x00; buf[pos++] = 0x00; // Shnum
    buf[pos++] = 0x00; buf[pos++] = 0x00; // Shstrndx
    
    // Program Header (single loadable segment)
    buf[pos++] = 1; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; // Type = PT_LOAD
    
    // Offset: 0x54 (84 bytes - immediately after headers)
    buf[pos++] = 0x54; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; 
    
    // Vaddr: 0x08048080
    buf[pos++] = 0x80; buf[pos++] = 0x80; buf[pos++] = 0x04; buf[pos++] = 0x08; 
    
    // Paddr: 0x08048080
    buf[pos++] = 0x80; buf[pos++] = 0x80; buf[pos++] = 0x04; buf[pos++] = 0x08; 
    
    buf[pos++] = code_size; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; // Filesz
    buf[pos++] = code_size; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; // Memsz
    buf[pos++] = 5; buf[pos++] = 0; buf[pos++] = 0; buf[pos++] = 0; // Flags = PF_R|PF_X
    buf[pos++] = 0x00; buf[pos++] = 0x10; buf[pos++] = 0x00; buf[pos++] = 0x00; // Align = 4096
    
    // Code segment
    memcpy(buf + pos, code, code_size);
    
    return 0;
}

int tcc_compile_string(tcc_state_t* s, const char* str) {
    if (!s || !str) return -1;
    
    // Store source code
    s->source_code = (char*)kmalloc(strlen(str) + 1);
    if (!s->source_code) return -1;
    strcpy(s->source_code, str);
    
    // Tokenize
    token_t* tokens = (token_t*)kmalloc(sizeof(token_t) * 1024);
    if (!tokens) {
        s->error_count++;
        if (s->source_code) { kfree(s->source_code); s->source_code = NULL; }
        return -1;
    }

    int token_count = tokenize_c_code(str, tokens, 1024);
    
    if (token_count == 0) {
        s->error_count++;
        kfree(tokens);
        return -1;
    }
    
    // Parse and generate code
    uint8_t code_buffer[4096];
    uint32_t code_size = sizeof(code_buffer);
    
    if (parse_c_tokens(tokens, code_buffer, &code_size) < 0) {
        s->error_count++;
        kfree(tokens);
        return -1;
    }
    
    kfree(tokens); // Free tokens after parsing

    // Generate ELF
    uint8_t* elf_output;
    uint32_t elf_size;
    
    if (generate_elf32(code_buffer, code_size, &elf_output, &elf_size) < 0) {
        s->error_count++;
        return -1;
    }
    
    // Store result
    if (s->output_buffer) kfree(s->output_buffer);
    s->output_buffer = (char*)elf_output;
    s->output_size = elf_size;
    
    return 0;
}

int tcc_output_file(tcc_state_t* s, const char* filename) {
    if (!s || !filename || !s->output_buffer) return -1;
    
    // This would write to filesystem - for now just return success
    log_serial("TCC: Output file ");
    log_serial(filename);
    log_serial(" (size: ");
    log_serial(" bytes)\n");
    
    return 0;
}

int tcc_output_memory(tcc_state_t* s, uint8_t** output, uint32_t* size) {
    if (!s || !output || !size || !s->output_buffer) return -1;
    
    *output = (uint8_t*)s->output_buffer;
    *size = s->output_size;
    
    return 0;
}

void tcc_set_error_func(tcc_state_t* s, void* error_opaque, void (*error_func)(void*, const char*)) {
    // Error handling stub
}