#include "string.h"

void* memset(void* dst, int value, uint32_t count) {
    uint8_t* d = (uint8_t*)dst;
    while (count--) *d++ = (uint8_t)value;
    return dst;
}

void* memcpy(void* dst, const void* src, uint32_t count) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (count--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, uint32_t count) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        while (count--) *d++ = *s++;
    } else {
        d += count;
        s += count;
        while (count--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void* a, const void* b, uint32_t count) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    while (count--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

uint32_t strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, uint32_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++)) { /* copy incl. NUL */ }
    return dst;
}

char* strncpy(char* dst, const char* src, uint32_t n) {
    uint32_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

uint32_t u32_to_dec(char* buf, uint32_t value) {
    char tmp[10];
    uint32_t c = 0, p = 0;
    if (!value) {
        buf[p++] = '0';
    } else {
        while (value) {
            tmp[c++] = (char)('0' + (value % 10));
            value /= 10;
        }
        while (c) buf[p++] = tmp[--c];
    }
    buf[p] = '\0';
    return p;
}
