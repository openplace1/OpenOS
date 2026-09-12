#pragma once

// Minimal stand-in for Arduino's String so the portable parsers under
// src/Runtime compile on a desktop. Only the subset those parsers use is
// implemented; semantics follow arduino-esp32's WString (indexOf returns -1,
// substring clamps, concat/reserve report success).
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

class String {
public:
    String() {}
    String(const char* text) : data(text ? text : "") {}
    String(const std::string& text) : data(text) {}
    String(const String& other) = default;
    String(String&& other) noexcept : data(std::move(other.data)) {}
    explicit String(char c) : data(1, c) {}
    explicit String(int value) { data = std::to_string(value); }
    explicit String(unsigned int value) { data = std::to_string(value); }
    explicit String(long value) { data = std::to_string(value); }
    explicit String(unsigned long value) { data = std::to_string(value); }

    String& operator=(const String& other) = default;
    String& operator=(String&& other) noexcept { data = std::move(other.data); return *this; }
    String& operator=(const char* text) { data = text ? text : ""; return *this; }

    unsigned int length() const { return (unsigned int)data.size(); }
    const char* c_str() const { return data.c_str(); }
    char operator[](int index) const {
        return (index < 0 || index >= (int)data.size()) ? 0 : data[(size_t)index];
    }
    char charAt(int index) const { return (*this)[index]; }

    bool reserve(unsigned int size) { data.reserve(size); return true; }
    bool concat(char c) { data.push_back(c); return true; }
    bool concat(const char* text) { if (text) data += text; return true; }
    bool concat(const char* text, unsigned int count) { data.append(text, count); return true; }
    bool concat(const String& other) { data += other.data; return true; }
    String& operator+=(const String& other) { data += other.data; return *this; }
    String& operator+=(const char* text) { if (text) data += text; return *this; }
    String& operator+=(char c) { data.push_back(c); return *this; }

    bool startsWith(const char* prefix) const {
        return prefix && data.compare(0, strlen(prefix), prefix) == 0 &&
               data.size() >= strlen(prefix);
    }
    bool startsWith(const String& prefix) const { return startsWith(prefix.c_str()); }
    bool endsWith(const char* suffix) const {
        size_t n = strlen(suffix);
        return data.size() >= n && data.compare(data.size() - n, n, suffix) == 0;
    }
    bool endsWith(const String& suffix) const { return endsWith(suffix.c_str()); }

    int indexOf(char c, unsigned int from = 0) const {
        size_t at = data.find(c, from);
        return at == std::string::npos ? -1 : (int)at;
    }
    int indexOf(const char* text, unsigned int from = 0) const {
        size_t at = data.find(text, from);
        return at == std::string::npos ? -1 : (int)at;
    }
    int indexOf(const String& text, unsigned int from = 0) const {
        return indexOf(text.c_str(), from);
    }
    int lastIndexOf(char c) const {
        size_t at = data.rfind(c);
        return at == std::string::npos ? -1 : (int)at;
    }
    int lastIndexOf(const char* text) const {
        size_t at = data.rfind(text);
        return at == std::string::npos ? -1 : (int)at;
    }
    int lastIndexOf(const String& text) const { return lastIndexOf(text.c_str()); }

    String substring(unsigned int from) const { return substring(from, length()); }
    String substring(unsigned int from, unsigned int to) const {
        if (from > to) { unsigned int t = from; from = to; to = t; }
        if (from >= data.size()) return String();
        if (to > data.size()) to = (unsigned int)data.size();
        return String(data.substr(from, to - from));
    }
    void remove(unsigned int index) { if (index < data.size()) data.erase(index); }
    void remove(unsigned int index, unsigned int count) {
        if (index < data.size()) data.erase(index, count);
    }
    void trim() {
        size_t start = 0;
        while (start < data.size() && (unsigned char)data[start] <= ' ') ++start;
        size_t end = data.size();
        while (end > start && (unsigned char)data[end - 1] <= ' ') --end;
        data = data.substr(start, end - start);
    }
    void toLowerCase() { for (char& c : data) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); }

    bool equals(const String& other) const { return data == other.data; }
    bool equals(const char* text) const { return text && data == text; }
    bool operator==(const String& other) const { return equals(other); }
    bool operator==(const char* text) const { return equals(text); }
    bool operator!=(const String& other) const { return !equals(other); }
    bool operator!=(const char* text) const { return !equals(text); }

    std::string data;
};

inline String operator+(const String& left, const String& right) {
    String result(left);
    result.concat(right);
    return result;
}
inline String operator+(const String& left, const char* right) {
    String result(left);
    result.concat(right);
    return result;
}
inline String operator+(const char* left, const String& right) {
    String result(left);
    result.concat(right);
    return result;
}
