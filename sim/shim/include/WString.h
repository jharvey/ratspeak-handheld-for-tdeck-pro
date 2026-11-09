#pragma once
// Arduino String on top of std::string (host shim).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

class __FlashStringHelper;

class String {
public:
    String() = default;
    String(const char* s) : s_(s ? s : "") {}
    String(const char* s, size_t n) : s_(s ? std::string(s, n) : std::string()) {}
    String(const std::string& s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(unsigned char v, unsigned char base = 10) { fromU(v, base); }
    String(int v, unsigned char base = 10) { fromI(v, base); }
    String(unsigned int v, unsigned char base = 10) { fromU(v, base); }
    String(long v, unsigned char base = 10) { fromI(v, base); }
    String(unsigned long v, unsigned char base = 10) { fromU(v, base); }
    String(long long v, unsigned char base = 10) { fromI(v, base); }
    String(unsigned long long v, unsigned char base = 10) { fromU(v, base); }
    String(float v, unsigned int dec = 2) { fromF(v, dec); }
    String(double v, unsigned int dec = 2) { fromF(v, dec); }

    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    bool reserve(size_t n) { s_.reserve(n); return true; }
    void clear() { s_.clear(); }
    char charAt(size_t i) const { return i < s_.size() ? s_[i] : 0; }
    void setCharAt(size_t i, char c) { if (i < s_.size()) s_[i] = c; }
    char operator[](size_t i) const { return charAt(i); }
    char& operator[](size_t i) { return s_[i]; }
    explicit operator bool() const { return true; }
    const std::string& str() const { return s_; }
    char* begin() { return &s_[0]; }
    char* end() { return &s_[0] + s_.size(); }
    const char* begin() const { return s_.data(); }
    const char* end() const { return s_.data() + s_.size(); }

    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    String& operator+=(const char* o) { if (o) s_ += o; return *this; }
    String& operator+=(char c) { s_ += c; return *this; }
    String& operator+=(int v) { return *this += String(v); }
    String& operator+=(unsigned int v) { return *this += String(v); }
    String& operator+=(long v) { return *this += String(v); }
    String& operator+=(unsigned long v) { return *this += String(v); }
    bool concat(const String& o) { s_ += o.s_; return true; }
    bool concat(const char* o) { if (o) s_ += o; return true; }
    bool concat(char c) { s_ += c; return true; }
    bool concat(const char* o, size_t n) { if (o) s_.append(o, n); return true; }

    bool equals(const String& o) const { return s_ == o.s_; }
    bool equalsIgnoreCase(const String& o) const {
        if (s_.size() != o.s_.size()) return false;
        for (size_t i = 0; i < s_.size(); i++)
            if (tolower((unsigned char)s_[i]) != tolower((unsigned char)o.s_[i])) return false;
        return true;
    }
    int compareTo(const String& o) const { return s_.compare(o.s_); }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    bool endsWith(const String& p) const {
        return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
    }
    int indexOf(char c, size_t from = 0) const { auto p = s_.find(c, from); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String& t, size_t from = 0) const { auto p = s_.find(t.s_, from); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = s_.rfind(c); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(const String& t) const { auto p = s_.rfind(t.s_); return p == std::string::npos ? -1 : (int)p; }
    String substring(size_t from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(size_t from, size_t to) const {
        if (from > to) std::swap(from, to);
        if (from >= s_.size()) return String();
        return String(s_.substr(from, to - from));
    }
    void trim() {
        size_t a = 0, b = s_.size();
        while (a < b && isspace((unsigned char)s_[a])) a++;
        while (b > a && isspace((unsigned char)s_[b - 1])) b--;
        s_ = s_.substr(a, b - a);
    }
    void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
    void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
    void replace(const String& a, const String& b) {
        if (a.s_.empty()) return;
        size_t p = 0;
        while ((p = s_.find(a.s_, p)) != std::string::npos) { s_.replace(p, a.s_.size(), b.s_); p += b.s_.size(); }
    }
    void remove(size_t i) { if (i < s_.size()) s_.erase(i); }
    void remove(size_t i, size_t n) { if (i < s_.size()) s_.erase(i, n); }
    long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
    float toFloat() const { return strtof(s_.c_str(), nullptr); }
    double toDouble() const { return strtod(s_.c_str(), nullptr); }
    void getBytes(unsigned char* buf, size_t n, size_t idx = 0) const {
        if (!n) return;
        size_t m = idx < s_.size() ? std::min(n - 1, s_.size() - idx) : 0;
        memcpy(buf, s_.data() + idx, m); buf[m] = 0;
    }
    void toCharArray(char* buf, size_t n, size_t idx = 0) const { getBytes((unsigned char*)buf, n, idx); }

    friend String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
    friend String operator+(const String& a, const char* b) { String r(a); r += b; return r; }
    friend String operator+(const char* a, const String& b) { String r(a); r += b; return r; }
    friend String operator+(const String& a, char b) { String r(a); r += b; return r; }
    friend bool operator==(const String& a, const String& b) { return a.s_ == b.s_; }
    friend bool operator==(const String& a, const char* b) { return a.s_ == (b ? b : ""); }
    friend bool operator!=(const String& a, const String& b) { return a.s_ != b.s_; }
    friend bool operator!=(const String& a, const char* b) { return a.s_ != (b ? b : ""); }
    friend bool operator<(const String& a, const String& b) { return a.s_ < b.s_; }

private:
    template <typename T> void fromU(T v, unsigned base) {
        char b[72]; int i = 70; b[71] = 0;
        if (base < 2) base = 10;
        if (!v) b[i--] = '0';
        while (v) { int d = (int)(v % base); b[i--] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; }
        s_ = &b[i + 1];
    }
    template <typename T> void fromI(T v, unsigned base) {
        if (v < 0 && base == 10) { fromU((unsigned long long)(-(long long)v), base); s_.insert(0, 1, '-'); }
        else fromU((unsigned long long)v, base);
    }
    void fromF(double v, unsigned dec) { char b[64]; snprintf(b, sizeof(b), "%.*f", (int)dec, v); s_ = b; }
    std::string s_;
};
