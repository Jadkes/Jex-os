#include "stdio.h"
#include "jexos.h"
#include "string.h"

static void itoa(int n, char* str, int base) {
    int i = 0;
    int is_neg = 0;
    if (n == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }
    if (n < 0 && base == 10) {
        is_neg = 1;
        n = -n;
    }
    while (n != 0) {
        int rem = n % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        n = n / base;
    }
    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    
    // Reverse
    int start = 0;
    int end = i - 1;
    while(start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++; end--;
    }
}

void printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    char buffer[1024]; // Temp buffer for formatting
    int buf_idx = 0;
    
    for (const char* p = format; *p != '\0'; p++) {
        if (*p != '%') {
            buffer[buf_idx++] = *p;
            continue;
        }
        
        p++; // skip %
        switch (*p) {
            case 's': {
                char* s = va_arg(args, char*);
                while(*s) buffer[buf_idx++] = *s++;
                break;
            }
            case 'd': {
                int d = va_arg(args, int);
                char tmp[16];
                itoa(d, tmp, 10);
                for(int j=0; tmp[j]; j++) buffer[buf_idx++] = tmp[j];
                break;
            }
            case 'x': {
                int x = va_arg(args, int);
                char tmp[16];
                itoa(x, tmp, 16);
                for(int j=0; tmp[j]; j++) buffer[buf_idx++] = tmp[j];
                break;
            }
            case '%': {
                buffer[buf_idx++] = '%';
                break;
            }
        }
    }
    buffer[buf_idx] = '\0';
    sys_print(buffer);
    va_end(args);
}

int fopen(const char* filename, const char* mode) {
    (void)mode; // Mode ignored for now (simple FAT12)
    return sys_open(filename, 0);
}

void fclose(int fd) {
    sys_close(fd);
}

int fread(void* ptr, size_t size, size_t nmemb, int fd) {
    return sys_read(fd, ptr, size * nmemb);
}

int fwrite(const void* ptr, size_t size, size_t nmemb, int fd) {
    return sys_write(fd, ptr, size * nmemb);
}
