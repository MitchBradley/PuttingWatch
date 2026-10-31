#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
using String = std::string;
class __FlashStringHelper;
class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t* b, size_t n) { size_t k = 0; while (n--) k += write(*b++); return k; }
    size_t write(const char* s) { return write((const uint8_t*)s, strlen(s)); }
    size_t print(const char* s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t println(const char* s) { size_t n = write(s); n += write((uint8_t)'\n'); return n; }
    size_t println() { return write((uint8_t)'\n'); }
};
