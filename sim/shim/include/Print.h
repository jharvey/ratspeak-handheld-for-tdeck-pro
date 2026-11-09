#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "WString.h"

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print;
class Printable {
public:
    virtual ~Printable() = default;
    virtual size_t printTo(Print& p) const = 0;
};

class Print {
public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t* b, size_t n) {
        size_t k = 0; while (n--) k += write(*b++); return k;
    }
    size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
    size_t write(const char* b, size_t n) { return write((const uint8_t*)b, n); }
    virtual void flush() {}
    virtual int availableForWrite() { return 4096; }

    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        if ((size_t)n < sizeof(buf)) return write((const uint8_t*)buf, (size_t)n);
        std::string big((size_t)n + 1, '\0');
        va_start(ap, fmt); vsnprintf(&big[0], big.size(), fmt, ap); va_end(ap);
        return write((const uint8_t*)big.data(), (size_t)n);
    }
    size_t print(const String& s) { return write(s.c_str(), s.length()); }
    size_t print(const char* s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v, int base = DEC) { return print(String((long)v, (unsigned char)base)); }
    size_t print(unsigned v, int base = DEC) { return print(String((unsigned long)v, (unsigned char)base)); }
    size_t print(long v, int base = DEC) { return print(String(v, (unsigned char)base)); }
    size_t print(unsigned long v, int base = DEC) { return print(String(v, (unsigned char)base)); }
    size_t print(long long v, int base = DEC) { return print(String(v, (unsigned char)base)); }
    size_t print(unsigned long long v, int base = DEC) { return print(String(v, (unsigned char)base)); }
    size_t print(unsigned char v, int base = DEC) { return print(String((unsigned long)v, (unsigned char)base)); }
    size_t print(double v, int dec = 2) { return print(String(v, (unsigned)dec)); }
    size_t println() { return write("\n"); }
    template <typename T> size_t println(const T& v) { size_t n = print(v); return n + println(); }
    template <typename T> size_t println(const T& v, int f) { size_t n = print(v, f); return n + println(); }
};
