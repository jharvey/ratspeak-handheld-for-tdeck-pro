#pragma once
// NVS shim: one file per namespace under <data>/nvs/.
#include <string>
#include "Arduino.h"
typedef enum { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB,
               PT_INVALID } PreferenceType;
class Preferences {
public:
    bool begin(const char* name, bool readOnly = false, const char* partition = nullptr);
    void end();
    bool clear();
    bool remove(const char* key);
    bool isKey(const char* key);
    PreferenceType getType(const char* key);
    size_t freeEntries() { return 400; }

    size_t putChar(const char* k, int8_t v) { return putNum(k, PT_I8, (int64_t)v, 1); }
    size_t putUChar(const char* k, uint8_t v) { return putNum(k, PT_U8, (int64_t)v, 1); }
    size_t putShort(const char* k, int16_t v) { return putNum(k, PT_I16, (int64_t)v, 2); }
    size_t putUShort(const char* k, uint16_t v) { return putNum(k, PT_U16, (int64_t)v, 2); }
    size_t putInt(const char* k, int32_t v) { return putNum(k, PT_I32, (int64_t)v, 4); }
    size_t putUInt(const char* k, uint32_t v) { return putNum(k, PT_U32, (int64_t)v, 4); }
    size_t putLong(const char* k, int32_t v) { return putInt(k, v); }
    size_t putULong(const char* k, uint32_t v) { return putUInt(k, v); }
    size_t putLong64(const char* k, int64_t v) { return putNum(k, PT_I64, v, 8); }
    size_t putULong64(const char* k, uint64_t v) { return putNum(k, PT_U64, (int64_t)v, 8); }
    size_t putFloat(const char* k, float v);
    size_t putDouble(const char* k, double v);
    size_t putBool(const char* k, bool v) { return putUChar(k, v ? 1 : 0); }
    size_t putString(const char* k, const char* v);
    size_t putString(const char* k, const String& v) { return putString(k, v.c_str()); }
    size_t putBytes(const char* k, const void* v, size_t n);

    int8_t getChar(const char* k, int8_t d = 0) { return (int8_t)getNum(k, d); }
    uint8_t getUChar(const char* k, uint8_t d = 0) { return (uint8_t)getNum(k, d); }
    int16_t getShort(const char* k, int16_t d = 0) { return (int16_t)getNum(k, d); }
    uint16_t getUShort(const char* k, uint16_t d = 0) { return (uint16_t)getNum(k, d); }
    int32_t getInt(const char* k, int32_t d = 0) { return (int32_t)getNum(k, d); }
    uint32_t getUInt(const char* k, uint32_t d = 0) { return (uint32_t)getNum(k, d); }
    int32_t getLong(const char* k, int32_t d = 0) { return getInt(k, d); }
    uint32_t getULong(const char* k, uint32_t d = 0) { return getUInt(k, d); }
    int64_t getLong64(const char* k, int64_t d = 0) { return getNum(k, d); }
    uint64_t getULong64(const char* k, uint64_t d = 0) { return (uint64_t)getNum(k, (int64_t)d); }
    float getFloat(const char* k, float d = NAN);
    double getDouble(const char* k, double d = NAN);
    bool getBool(const char* k, bool d = false) { return getNum(k, d ? 1 : 0) != 0; }
    size_t getString(const char* k, char* out, size_t max);
    String getString(const char* k, const String& d = String());
    size_t getBytesLength(const char* k);
    size_t getBytes(const char* k, void* out, size_t max);

private:
    size_t putNum(const char* k, PreferenceType t, int64_t v, size_t sz);
    int64_t getNum(const char* k, int64_t d);
    bool started_ = false, ro_ = false;
    std::string ns_;
};
