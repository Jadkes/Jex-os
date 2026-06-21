/**
 * @file expr.c
 * @brief Expression evaluator for TCC compiler using precedence climbing.
 *
 * Purpose: Parse and generate x86 code for C expressions in O(n) time.
 * Design: Precedence climbing (NOT recursive descent for all levels).
 *          Generates code directly without AST storage (single-pass TCC).
 * Thread-safety: Not applicable (single-threaded compiler).
 *
 * Key design decisions:
 * - NO AST: TCC is single-pass, generate x86 directly during parsing
 * - Result always in eax register
 * - Binary ops: push left, eval right, pop left to ebx, compute, result in eax
 * - Prefix ops: handle & (lea), * (dereference), unary +/-/!/~
 * - Variables: lookup symtab, get stack offset, generate mov eax, [ebp-offset]
 */

#include "expr.h"
#include "tcc.h"  /* For func_entry_t, MAX_FUNCS, lookup_function */
#include <string.h>
#include <stdio.h>

/* Forward declaration */
static int parse_expr_1(token_t* tokens, int* pos, symtab_t* tab,
                           int* lhs_parsed, int min_prec,
                           uint8_t* output, uint32_t* out_pos);

/* External symbol and relocation support (defined in tcc.c) */
extern uint32_t register_external_symbol(const char* name);
extern void add_relocation(uint32_t offset, uint32_t type, uint32_t sym_index);
extern int reloc_count;

/* Track last parsed identifier's type for pointer arithmetic */
static sym_type_t last_ident_type = SYM_INT;
/* Track last parsed identifier's base type size for pointer arithmetic */
static int last_ident_ptr_size = 4; /* default to int size */
/* Track if left operand of + is a pointer */
static int left_is_pointer = 0;

/* x86 opcode constants */
#define X86_MOV_EAX_IMM  0xB8
#define X86_PUSH_EAX     0x50
#define X86_POP_EBX      0x5B
#define X86_CDQ          0x99    /* sign-extend eax to edx:eax */
#define X86_XOR_EAX_EAX  0x33    /* xor eax, eax */
#define X86_INT_80       0xCD    /* int 0x80 */

/**
 * Get precedence level for binary operators.
 * Higher number = tighter binding.
 */
int expr_get_precedence(token_type_t type) {
    switch (type) {
        case TOK_PLUS:
        case TOK_MINUS:  return PREC_ADD;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT: return PREC_MUL;
        case TOK_SHL:
        case TOK_SHR:    return PREC_SHIFT;
        case TOK_LT:
        case TOK_GT:
        case TOK_LE:
        case TOK_GE:     return PREC_REL;
        case TOK_EQ:
        case TOK_NEQ:    return PREC_EQ;
        case TOK_LOGAND:  return PREC_LOGAND;
        case TOK_LOGOR:   return PREC_LOGOR;
        default:           return PREC_NONE;
    }
}

int expr_is_binary_op(token_type_t type) {
    return expr_get_precedence(type) > PREC_NONE;
}

/**
 * Emit mov eax, imm32 instruction.
 */
static void emit_mov_eax_imm(uint8_t* buf, uint32_t* pos, uint32_t imm) {
    buf[(*pos)++] = X86_MOV_EAX_IMM;
    buf[(*pos)++] = imm & 0xFF;
    buf[(*pos)++] = (imm >> 8) & 0xFF;
    buf[(*pos)++] = (imm >> 16) & 0xFF;
    buf[(*pos)++] = (imm >> 24) & 0xFF;
}

/**
 * Emit push eax instruction.
 */
static void emit_push_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = X86_PUSH_EAX;
}

/**
 * Emit pop ebx instruction.
 */
static void emit_pop_ebx(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = X86_POP_EBX;
}

/**
 * Emit mov eax, [ebp-offset] instruction.
 * Load variable value from stack (32-bit).
 */
static void emit_mov_eax_mem_ebp(uint8_t* buf, uint32_t* pos, int offset) {
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x8B; /* mov r32, r/m32 */
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x8B; /* mov r32, r/m32 */
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset); /* negative offset (already negative) */
    }
}

/**
 * Emit movsx eax, byte [ebp-offset] instruction.
 * Load char variable with sign extension (8-bit to 32-bit).
 */
static void emit_movsx_eax_mem8_ebp(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x0F; /* movsx prefix */
    buf[(*pos)++] = 0xBE; /* movsx r32, r/m8 */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
}

/**
 * Emit lea eax, [ebp-offset] instruction.
 * Load address of variable (for & operator).
 */
static void emit_lea_eax_ebp(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x8D; /* lea r32, m */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
}

/**
 * Emit mov eax, [eax] instruction.
 * Dereference pointer in eax.
 */
static void emit_mov_eax_mem_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x8B; /* mov eax, [eax] */
    buf[(*pos)++] = 0x00; /* modrm: [eax] */
}

/**
 * Emit not eax instruction.
 */
static void emit_not_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0xF7;
    buf[(*pos)++] = 0xD0; /* modrm: 11 010 000 */
}

/**
 * Emit neg eax instruction.
 */
static void emit_neg_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0xF7;
    buf[(*pos)++] = 0xD8; /* modrm: 11 011 000 */
}

/**
 * Emit add dword [ebp-offset], 1 instruction.
 * Increment a variable on the stack by 1.
 */
static void __attribute__((unused)) emit_add_mem_ebp_1(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x83; /* add r/m32, imm8 */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
    buf[(*pos)++] = 0x01; /* imm8 = 1 */
}

/**
 * Emit sub dword [ebp-offset], 1 instruction.
 * Decrement a variable on the stack by 1.
 */
static void __attribute__((unused)) emit_sub_mem_ebp_1(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x83; /* sub r/m32, imm8 */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
    buf[(*pos)++] = 0x01; /* imm8 = 1 */
}

/**
 * Emit mov ebx, eax instruction.
 */
static void __attribute__((unused)) emit_mov_ebx_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x89; /* mov r32, r32 */
    buf[(*pos)++] = 0xC3; /* modrm: ebx, eax */
}

/**
 * Emit add eax, ebx instruction.
 * Result in eax.
 */
static void emit_add_eax_ebx(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x03; /* add eax, ebx */
    buf[(*pos)++] = 0xC3; /* modrm: eax, ebx */
}

/**
 * Emit imul eax, imm32 instruction.
 * Multiply eax by immediate value.
 */
static void emit_imul_eax_imm(uint8_t* buf, uint32_t* pos, uint32_t imm) {
    buf[(*pos)++] = 0x69; /* imul eax, eax, imm32 */
    buf[(*pos)++] = 0xC0; /* modrm: eax, eax */
    *(uint32_t*)(buf + *pos) = imm;
    *pos += 4;
}

/**
 * Emit add eax, imm32 instruction.
 * Add immediate value to eax.
 */
static void emit_add_eax_imm(uint8_t* buf, uint32_t* pos, uint32_t imm) {
    buf[(*pos)++] = 0x05; /* add eax, imm32 */
    *(uint32_t*)(buf + *pos) = imm;
    *pos += 4;
}

/**
 * Emit mov [ebp-offset], eax instruction.
 * Store eax value to stack variable (32-bit).
 */
static void emit_mov_mem_ebp_eax(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x89; /* mov r/m32, r32 */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
}

/**
 * Emit mov [ebp-offset], al instruction (8-bit store for char).
 */
static void emit_mov_mem_ebp_al(uint8_t* buf, uint32_t* pos, int offset) {
    buf[(*pos)++] = 0x88; /* mov r/m8, r8 */
    if (offset < -128 || offset > 127) {
        buf[(*pos)++] = 0x85; /* modrm: [ebp+disp32] */
        *(int32_t*)(buf + *pos) = offset;
        *pos += 4;
    } else {
        buf[(*pos)++] = 0x45; /* modrm: [ebp+disp8] */
        buf[(*pos)++] = (uint8_t)(offset);
    }
}

/**
 * Emit xor eax, eax instruction.
 */
static void emit_xor_eax_eax(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = X86_XOR_EAX_EAX;
    buf[(*pos)++] = 0xC0; /* modrm: 11 000 000 */
}

/**
 * Emit cmp eax, ebx instruction.
 */
static void emit_cmp_eax_ebx(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x39; /* cmp eax, ebx */
    buf[(*pos)++] = 0xD8; /* modrm: 11 011 000 */
}

/**
 * Emit sete al instruction.
 */
static void emit_sete_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x94;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit setne al instruction.
 */
static void emit_setne_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x95;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit setl al instruction.
 */
static void emit_setl_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x9C;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit setg al instruction.
 */
static void emit_setg_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x9F;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit setle al instruction.
 */
static void emit_setle_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x9E;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit setge al instruction.
 */
static void emit_setge_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0x9D;
    buf[(*pos)++] = 0xC0;
}

/**
 * Emit movzx eax, al instruction.
 */
static void emit_movzx_eax_al(uint8_t* buf, uint32_t* pos) {
    buf[(*pos)++] = 0x0F;
    buf[(*pos)++] = 0xB6;
    buf[(*pos)++] = 0xC0; /* modrm: 11 000 000 */
}

/**
 * Parse a primary expression: literal, variable, parenthesized expr, prefix op.
 * Result in eax.
 */
static int parse_primary(token_t* tokens, int* pos, symtab_t* tab,
                         uint8_t* output, uint32_t* out_pos) {
    token_t tok = tokens[*pos];

    /* Number literal */
    if (tok.type == TOK_NUMBER) {
        emit_mov_eax_imm(output, out_pos, (uint32_t)tok.int_val);
        (*pos)++;
        return 0;
    }

    /* Identifier (variable or function call) */
    if (tok.type == TOK_IDENT) {
        /* Check if this is a function call */
        if (tokens[*pos + 1].type == TOK_LPAREN) {
            /* Function call: func_name(args) */
            uint32_t func_pos = lookup_function(tok.str);
            int is_external = 0;

            if (func_pos == 0) {
                /* External function - register for relocation */
                is_external = 1;
                func_pos = 0; /* Placeholder for now */
            }

            (*pos) += 2; /* Skip func_name and '(' */

            /* Push arguments right-to-left */
            int arg_sizes = 0;

            if (tokens[*pos].type != TOK_RPAREN) {
                /* Collect arguments first (to push right-to-left) */
#define MAX_FUNC_ARGS 32
                int arg_positions[MAX_FUNC_ARGS];
                int arg_count_temp = 0;

                while (tokens[*pos].type != TOK_RPAREN && tokens[*pos].type != TOK_EOF) {
                    if (arg_count_temp >= MAX_FUNC_ARGS) return -1; /* Too many args */
                    arg_positions[arg_count_temp++] = *pos;

                    /* Skip until next comma or ')' */
                    int paren_depth = 0;
                    while (tokens[*pos].type != TOK_RPAREN || paren_depth > 0) {
                        if (tokens[*pos].type == TOK_LPAREN) paren_depth++;
                        if (tokens[*pos].type == TOK_RPAREN && paren_depth > 0) paren_depth--;
                        if (tokens[*pos].type == TOK_COMMA && paren_depth == 0) break;
                        (*pos)++;
                    }
                    if (tokens[*pos].type == TOK_COMMA) (*pos)++;
                }

                /* Now push arguments right-to-left */
                for (int j = arg_count_temp - 1; j >= 0; j--) {
                    int temp_pos = arg_positions[j];
                    int lhs_parsed = 0;
                    if (parse_expr_1(tokens, &temp_pos, tab, &lhs_parsed,
                                    PREC_NONE, output, out_pos) < 0) {
                        return -1;
                    }
                    /* Push argument onto stack */
                    output[(*out_pos)++] = 0x50; /* push eax */
                    arg_sizes += 4; /* All args are 4 bytes (int) */
                }
            }

            if (tokens[*pos].type == TOK_RPAREN) (*pos)++;

            /* Call function */
            uint32_t start_pos = *out_pos;
            output[(*out_pos)++] = 0xE8; /* call rel32 */

            if (is_external) {
                /* External function: emit placeholder, record relocation */
                *(uint32_t*)(output + *out_pos) = 0x00000000; /* Placeholder */
                uint32_t sym_index = register_external_symbol(tok.str);
                add_relocation(start_pos + 1, 2, sym_index); /* R_386_PC32 = 2 */
            } else {
                /* Internal function: calculate relative offset */
                uint32_t rel32 = func_pos - (start_pos + 5);
                *(uint32_t*)(output + *out_pos) = rel32;
            }
            *out_pos += 4;

            /* Clean up stack (caller cleans up in cdecl) */
            if (arg_sizes > 0) {
                output[(*out_pos)++] = 0x81; /* add esp, imm32 */
                output[(*out_pos)++] = 0xC4; /* modrm: esp */
                *(uint32_t*)(output + *out_pos) = arg_sizes;
                *out_pos += 4;
            }

            /* Result is in eax */
            return 0;
        }

        if (tokens[*pos + 1].type == TOK_LBRACKET) {
            symbol_t* sym = symtab_lookup(tab, tok.str);
            if (!sym || sym->array_size <=0) {
                return -1;
            }

            (*pos) += 2;

            if (expr_parse(tokens, pos, tab, output, out_pos) < 0) {
                return -1;
            }

            output[(*out_pos)++] = 0x50;

            output[(*out_pos)++] = 0x8D;
            output[(*out_pos)++] = 0x45;
            output[(*out_pos)++] = (uint8_t)(-sym->offset);

            output[(*out_pos)++] = 0x5B;

            int elem_size = sym_type_size(sym->base_type);
            if (elem_size == 4) {
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0xE3;
                output[(*out_pos)++] = 0x02; /* shl ebx, 2 */
            } else if (elem_size == 2) {
                output[(*out_pos)++] = 0xD1;
                output[(*out_pos)++] = 0xE3;
                output[(*out_pos)++] = 0x01; /* shl ebx, 1 */
            }
            /* elem_size == 1 (char): no shift needed */

            output[(*out_pos)++] = 0x01;
            output[(*out_pos)++] = 0xD8;

            output[(*out_pos)++] = 0x8B;
            output[(*out_pos)++] = 0x00;

            if (tokens[*pos].type == TOK_RBRACKET) (*pos)++;

            return 0;
        }

        /* Not a function call - treat as variable */
        symbol_t* sym = symtab_lookup(tab, tok.str);
        if (!sym) {
            return -1;  /* Undefined variable */
        }
        /* Track type for pointer arithmetic */
        last_ident_type = sym->type;
        /* Track pointer base type size for pointer arithmetic */
        if (sym->type == SYM_PTR && sym->base_type) {
            last_ident_ptr_size = sym_type_size(sym->base_type);
        } else {
            last_ident_ptr_size = 4; /* default to int size */
        }
        /* Use sign extension for char types */
        if (sym->type == SYM_CHAR) {
            emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
        } else {
            emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
        }
        (*pos)++;

        /* Check for post-increment (i++) or post-decrement (i--) */
        if (tokens[*pos].type == TOK_PLUSPLUS) {
            /* Post-increment: return old value, then increment */
            if (sym->type == SYM_CHAR) {
                /* char: load sign-extended, push, add 1, store byte */
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
                emit_push_eax(output, out_pos);
                emit_add_eax_imm(output, out_pos, 1);
                emit_mov_mem_ebp_al(output, out_pos, sym->offset);
            } else {
                emit_push_eax(output, out_pos); /* push old value */
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
                if (sym->type == SYM_PTR && sym->base_type) {
                    int size = sym_type_size(sym->base_type);
                    emit_add_eax_imm(output, out_pos, size);
                } else {
                    emit_add_eax_imm(output, out_pos, 1);
                }
                emit_mov_mem_ebp_eax(output, out_pos, sym->offset);
            }
            /* Pop old value into eax (this is the result) */
            output[(*out_pos)++] = 0x58; /* pop eax */
            (*pos)++;
        } else if (tokens[*pos].type == TOK_MINUSMINUS) {
            /* Post-decrement: return old value, then decrement */
            if (sym->type == SYM_CHAR) {
                /* char: load sign-extended, push, sub 1, store byte */
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
                emit_push_eax(output, out_pos);
                output[(*out_pos)++] = 0x83; /* sub eax, imm8 */
                output[(*out_pos)++] = 0xE8;
                output[(*out_pos)++] = 0x01;
                emit_mov_mem_ebp_al(output, out_pos, sym->offset);
            } else {
                emit_push_eax(output, out_pos); /* push old value */
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
                if (sym->type == SYM_PTR && sym->base_type) {
                    int size = sym_type_size(sym->base_type);
                    output[(*out_pos)++] = 0x81; /* sub eax, imm32 */
                    output[(*out_pos)++] = 0xE8;
                    *(uint32_t*)(output + *out_pos) = size;
                    *out_pos += 4;
                } else {
                    output[(*out_pos)++] = 0x83; /* sub eax, imm8 */
                    output[(*out_pos)++] = 0xE8;
                    output[(*out_pos)++] = 0x01;
                }
                emit_mov_mem_ebp_eax(output, out_pos, sym->offset);
            }
            /* Pop old value into eax (this is the result) */
            output[(*out_pos)++] = 0x58; /* pop eax */
            (*pos)++;
        }
        return 0;
    }

    /* Parenthesized expression */
    if (tok.type == TOK_LPAREN) {
        (*pos)++;  /* Skip '(' */
        if (expr_parse(tokens, pos, tab, output, out_pos) < 0) {
            return -1;
        }
        if (tokens[*pos].type != TOK_RPAREN) {
            return -1;  /* Expected ')' */
        }
        (*pos)++;  /* Skip ')' */
        return 0;
    }

    /* Prefix operators: +, -, !, ~, &, * (deref), ++, -- */
    if (tok.type == TOK_PLUS || tok.type == TOK_MINUS ||
        tok.type == TOK_LOGNOT || tok.type == TOK_NOT ||
        tok.type == TOK_AND || tok.type == TOK_STAR ||
        tok.type == TOK_PLUSPLUS || tok.type == TOK_MINUSMINUS) {
        
        token_type_t prefix_op = tok.type;
        (*pos)++;  /* Skip prefix operator */
        
        /* Handle & (address-of) specially - don't parse as value */
        if (prefix_op == TOK_AND) {
            if (tokens[*pos].type != TOK_IDENT) return -1;
            symbol_t* sym = symtab_lookup(tab, tokens[*pos].str);
            if (!sym) return -1;
            emit_lea_eax_ebp(output, out_pos, sym->offset);
            (*pos)++;
            return 0;
        }
        
        /* Handle prefix ++i (pre-increment) */
        if (prefix_op == TOK_PLUSPLUS) {
            if (tokens[*pos].type != TOK_IDENT) return -1;
            symbol_t* sym = symtab_lookup(tab, tokens[*pos].str);
            if (!sym) return -1;
            if (sym->type == SYM_CHAR) {
                /* char: load, add 1, store byte, load sign-extended */
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
                emit_add_eax_imm(output, out_pos, 1);
                emit_mov_mem_ebp_al(output, out_pos, sym->offset);
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
            } else {
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
                if (sym->type == SYM_PTR && sym->base_type) {
                    int size = sym_type_size(sym->base_type);
                    emit_add_eax_imm(output, out_pos, size);
                } else {
                    emit_add_eax_imm(output, out_pos, 1);
                }
                emit_mov_mem_ebp_eax(output, out_pos, sym->offset);
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
            }
            (*pos)++;
            return 0;
        }
        
        /* Handle prefix --i (pre-decrement) */
        if (prefix_op == TOK_MINUSMINUS) {
            if (tokens[*pos].type != TOK_IDENT) return -1;
            symbol_t* sym = symtab_lookup(tab, tokens[*pos].str);
            if (!sym) return -1;
            if (sym->type == SYM_CHAR) {
                /* char: load, sub 1, store byte, load sign-extended */
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
                output[(*out_pos)++] = 0x83; /* sub eax, imm8 */
                output[(*out_pos)++] = 0xE8;
                output[(*out_pos)++] = 0x01;
                emit_mov_mem_ebp_al(output, out_pos, sym->offset);
                emit_movsx_eax_mem8_ebp(output, out_pos, sym->offset);
            } else {
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
                if (sym->type == SYM_PTR && sym->base_type) {
                    int size = sym_type_size(sym->base_type);
                    output[(*out_pos)++] = 0x81; /* sub eax, imm32 */
                    output[(*out_pos)++] = 0xE8; /* modrm: eax */
                    *(uint32_t*)(output + *out_pos) = size;
                    *out_pos += 4;
                } else {
                    output[(*out_pos)++] = 0x83; /* sub eax, imm8 */
                    output[(*out_pos)++] = 0xE8; /* modrm: eax */
                    output[(*out_pos)++] = 0x01;
                }
                emit_mov_mem_ebp_eax(output, out_pos, sym->offset);
                emit_mov_eax_mem_ebp(output, out_pos, sym->offset);
            }
            (*pos)++;
            return 0;
        }
        
        /* For * (dereference), we need to parse the pointer expression */
        if (prefix_op == TOK_STAR) {
            /* Parse the pointer expression */
            if (parse_primary(tokens, pos, tab, output, out_pos) < 0) {
                return -1;
            }
            /* Dereference: eax contains pointer, load [eax] */
            emit_mov_eax_mem_eax(output, out_pos);
            return 0;
        }
        
        /* Parse the operand */
        if (parse_primary(tokens, pos, tab, output, out_pos) < 0) {
            return -1;
        }
        
        /* Apply unary operators */
        switch (prefix_op) {
            case TOK_PLUS:   /* unary +: no-op */
                break;
            case TOK_MINUS:  /* neg eax */
                emit_neg_eax(output, out_pos);
                break;
            case TOK_LOGNOT: /* !: set eax to 1 if zero, 0 if non-zero */
                emit_push_eax(output, out_pos);
                emit_xor_eax_eax(output, out_pos); /* eax = 0 */
                emit_pop_ebx(output, out_pos); /* ebx = original */
                emit_cmp_eax_ebx(output, out_pos); /* cmp 0, original */
                emit_sete_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_NOT:   /* ~: not eax */
                emit_not_eax(output, out_pos);
                break;
            default:
                return -1;
        }
        return 0;
    }

    return -1;  /* Unexpected token */
}

/**
 * Parse expression with precedence climbing.
 * lhs_parsed: flag indicating if left-hand side already parsed (in eax).
 * min_prec: minimum precedence to continue parsing.
 */
static int parse_expr_1(token_t* tokens, int* pos, symtab_t* tab,
                          int* lhs_parsed, int min_prec,
                          uint8_t* output, uint32_t* out_pos) {
    /* Parse left-hand side if not already done */
    if (!*lhs_parsed) {
        if (parse_primary(tokens, pos, tab, output, out_pos) < 0) {
            return -1;
        }
        *lhs_parsed = 1;
    }

    /* Precedence climbing loop */
    while (expr_is_binary_op(tokens[*pos].type)) {
        token_type_t op = tokens[*pos].type;
        int op_prec = expr_get_precedence(op);

        if (op_prec < min_prec) {
            break;
        }

        (*pos)++;  /* Consume operator */

        /* Check if left operand is a pointer (for pointer arithmetic) */
        left_is_pointer = (last_ident_type == SYM_PTR) ? 1 : 0;

        /* Push left operand (in eax) onto stack */
        emit_push_eax(output, out_pos);

        /* Parse right operand */
        if (parse_primary(tokens, pos, tab, output, out_pos) < 0) {
            return -1;
        }

        /* Handle higher precedence operators on right side */
        while (expr_is_binary_op(tokens[*pos].type) &&
               expr_get_precedence(tokens[*pos].type) > op_prec) {
            /* Recursively parse at higher precedence */
            int recursive_lhs = 1; /* right operand is already in eax */
            if (parse_expr_1(tokens, pos, tab, &recursive_lhs,
                           expr_get_precedence(tokens[*pos].type),
                           output, out_pos) < 0) {
                return -1;
            }
        }

        /* Pop left operand into ebx */
        emit_pop_ebx(output, out_pos);

        /* Apply binary operator: left in ebx, right in eax, result in eax */
        switch (op) {
        case TOK_SHL:   /* ebx << eax -> result in eax */
            /* mov ecx, eax (shift count); mov eax, ebx (value); shl eax, cl */
            output[(*out_pos)++] = 0x89; /* mov ecx, eax */
            output[(*out_pos)++] = 0xC1;
            output[(*out_pos)++] = 0x89; /* mov eax, ebx */
            output[(*out_pos)++] = 0xD8;
            output[(*out_pos)++] = 0xD3; /* shl eax, cl */
            output[(*out_pos)++] = 0xE0;
            break;
        case TOK_SHR:   /* ebx >> eax -> result in eax */
            output[(*out_pos)++] = 0x89; /* mov ecx, eax */
            output[(*out_pos)++] = 0xC1;
            output[(*out_pos)++] = 0x89; /* mov eax, ebx */
            output[(*out_pos)++] = 0xD8;
            output[(*out_pos)++] = 0xD3; /* shr eax, cl */
            output[(*out_pos)++] = 0xE8;
            break;
        case TOK_PLUS:   /* eax = left + right = ebx + eax */
            /* Check for pointer arithmetic: pointer + int */
            if (left_is_pointer) {
                /* Scale right (eax) by pointer element size */
                if (last_ident_ptr_size > 1) {
                    emit_imul_eax_imm(output, out_pos, last_ident_ptr_size);
                }
                emit_add_eax_ebx(output, out_pos); /* eax = eax + ebx */
                left_is_pointer = 0; /* Reset flag */
            } else {
                output[(*out_pos)++] = 0x03; /* add eax, ebx */
                output[(*out_pos)++] = 0xC3; /* ModRM: eax, ebx */
            }
            break;
            case TOK_MINUS:  /* eax = left - right = ebx - eax */
                /* mov ecx, eax; mov eax, ebx; sub eax, ecx */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x29; /* sub eax, ecx */
                output[(*out_pos)++] = 0xC1;
                break;
            case TOK_STAR:   /* imul ebx; result in eax */
                output[(*out_pos)++] = 0x0F; /* imul */
                output[(*out_pos)++] = 0xAF;
                output[(*out_pos)++] = 0xC3; /* modrm: eax *= ebx */
                break;
            case TOK_SLASH:  /* cdq; idiv ebx */
                output[(*out_pos)++] = X86_CDQ; /* sign extend eax to edx:eax */
                output[(*out_pos)++] = 0xF7; /* idiv */
                output[(*out_pos)++] = 0xFB; /* modrm: idiv ebx */
                break;
            case TOK_PERCENT: /* cdq; idiv ebx; mov eax, edx */
                output[(*out_pos)++] = X86_CDQ;
                output[(*out_pos)++] = 0xF7;
                output[(*out_pos)++] = 0xFB; /* idiv ebx */
                output[(*out_pos)++] = 0x89; /* mov eax, edx */
                output[(*out_pos)++] = 0xD0;
                break;
            case TOK_LT:     /* cmp ebx, eax; setl al */
                /* Want: cmp left, right. left=ebx, right=eax */
                /* mov ecx, eax; mov eax, ebx; cmp eax, ecx */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_setl_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_GT:     /* cmp ebx, eax; setg al */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_setg_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_LE:     /* cmp ebx, eax; setle al */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_setle_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_GE:     /* cmp ebx, eax; setge al */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_setge_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_EQ:     /* cmp ebx, eax; sete al */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_sete_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_NEQ:    /* cmp ebx, eax; setne al */
                output[(*out_pos)++] = 0x89; /* mov ecx, eax */
                output[(*out_pos)++] = 0xC1;
                output[(*out_pos)++] = 0x89; /* mov eax, ebx */
                output[(*out_pos)++] = 0xD8;
                output[(*out_pos)++] = 0x39; /* cmp eax, ecx */
                output[(*out_pos)++] = 0xC1;
                emit_setne_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                break;
            case TOK_LOGAND: /* &&: if left==0 then 0 else right!=0 */
                /* test ebx, ebx; jz short skip */
                output[(*out_pos)++] = 0x85; /* test ebx, ebx */
                output[(*out_pos)++] = 0xDB;
                output[(*out_pos)++] = 0x74; /* jz */
                output[(*out_pos)++] = 0x05; /* skip 5 bytes */
                output[(*out_pos)++] = 0x85; /* test eax, eax */
                output[(*out_pos)++] = 0xC0;
                emit_setne_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                output[(*out_pos)++] = 0xEB; /* jmp */
                output[(*out_pos)++] = 0x03; /* skip 3 bytes */
                output[(*out_pos)++] = X86_XOR_EAX_EAX; /* xor eax, eax */
                output[(*out_pos)++] = 0xC0;
                break;
            case TOK_LOGOR:  /* ||: if left!=0 then 1 else right!=0 */
                output[(*out_pos)++] = 0x85; /* test ebx, ebx */
                output[(*out_pos)++] = 0xDB;
                output[(*out_pos)++] = 0x75; /* jnz */
                output[(*out_pos)++] = 0x05; /* skip 5 bytes */
                output[(*out_pos)++] = 0x85; /* test eax, eax */
                output[(*out_pos)++] = 0xC0;
                emit_setne_al(output, out_pos);
                emit_movzx_eax_al(output, out_pos);
                output[(*out_pos)++] = 0xEB; /* jmp */
                output[(*out_pos)++] = 0x03; /* skip 3 bytes */
                output[(*out_pos)++] = 0xB8; /* mov eax, 1 */
                output[(*out_pos)++] = 0x01;
                output[(*out_pos)++] = 0x00;
                output[(*out_pos)++] = 0x00;
                output[(*out_pos)++] = 0x00;
                break;
            default:
                return -1;
        }
    }

    return 0;
}

/**
 * Main expression parser entry point.
 * Uses precedence climbing algorithm.
 * Result left in eax register.
 */
int expr_parse(token_t* tokens, int* pos, symtab_t* tab,
               uint8_t* output, uint32_t* out_pos) {
    int lhs_parsed = 0; /* left-hand side not yet parsed */
    return parse_expr_1(tokens, pos, tab, &lhs_parsed,
                           PREC_NONE, output, out_pos);
}
