# JexOS Self-Hosting Guide

## Current TCC Capabilities

Your JexOS now has **self-hosting** capabilities with a basic TCC implementation. Here's what works:

### ✅ Working C Constructs:
- Simple functions: `int func() { return value; }`
- Basic return statements
- Integer literals
- Function definitions and calls

### ❌ Not Yet Supported:
- Variable declarations (`char* msg = "..."`)
- Inline assembly (`asm volatile("...")`)
- Complex expressions
- Preprocessor directives (`#include`, `#define`)
- String literals in code generation

## Working Examples

### Example 1: Simple Return Value
```c
int main() {
    return 42;
}
```

### Example 2: Basic Function Call
```c
int helper() {
    return 123;
}

int main() {
    return helper();
}
```

## Usage in JexOS

```bash
# Create a C file
root@jexos:/> vix simple.c

# Compile and run immediately
root@jexos:/> tcc simple.c

# Compile to ELF file
root@jexos:/> cc simple.c -o simple
root@jexos:/> ./simple
```

## Current Limitations

The TCC implementation is **basic** but functional. It supports:
- Tokenization of C syntax
- Simple code generation
- ELF32 output format
- Process execution

For **full C compilation**, you would need:
- Variable support
- String literals
- Inline assembly for syscalls
- Expression evaluation

## Debug Information

If you get "Invalid Opcode" panics:
1. Check your C code uses only supported constructs
2. Use simple `return value;` patterns
3. Avoid complex declarations

The system will show debug output for compilation process via serial console.