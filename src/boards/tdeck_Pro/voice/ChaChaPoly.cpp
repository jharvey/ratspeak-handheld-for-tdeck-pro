// ChaCha20-Poly1305 AEAD (RFC 8439). See ChaChaPoly.h.
#include "ChaChaPoly.h"

#include <string.h>

namespace chachapoly {
namespace {

inline uint32_t rotl(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define QR(a, b, c, d)                                                       \
    a += b; d ^= a; d = rotl(d, 16); c += d; b ^= c; b = rotl(b, 12);        \
    a += b; d ^= a; d = rotl(d, 8);  c += d; b ^= c; b = rotl(b, 7);

void chachaBlock(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = le32(nonce + 4 * i);
    uint32_t x[16];
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) put32(out + 4 * i, x[i] + s[i]);
}

void chachaXor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, size_t len,
               uint8_t* out) {
    uint8_t ks[64];
    while (len) {
        chachaBlock(key, counter++, nonce, ks);
        const size_t k = len < 64 ? len : 64;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k; out += k; len -= k;
    }
}

// Poly1305, 26-bit limbs (after poly1305-donna, public domain).
struct Poly {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    size_t left;
};

void polyInit(Poly& p, const uint8_t key[32]) {
    p.r[0] = (le32(key + 0)) & 0x3ffffff;
    p.r[1] = (le32(key + 3) >> 2) & 0x3ffff03;
    p.r[2] = (le32(key + 6) >> 4) & 0x3ffc0ff;
    p.r[3] = (le32(key + 9) >> 6) & 0x3f03fff;
    p.r[4] = (le32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p.h[i] = 0;
    for (int i = 0; i < 4; i++) p.pad[i] = le32(key + 16 + 4 * i);
    p.left = 0;
}

void polyBlocks(Poly& p, const uint8_t* m, size_t bytes, uint32_t hibit) {
    const uint32_t r0 = p.r[0], r1 = p.r[1], r2 = p.r[2], r3 = p.r[3], r4 = p.r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p.h[0], h1 = p.h[1], h2 = p.h[2], h3 = p.h[3], h4 = p.h[4];
    while (bytes >= 16) {
        h0 += (le32(m + 0)) & 0x3ffffff;
        h1 += (le32(m + 3) >> 2) & 0x3ffffff;
        h2 += (le32(m + 6) >> 4) & 0x3ffffff;
        h3 += (le32(m + 9) >> 6) & 0x3ffffff;
        h4 += (le32(m + 12) >> 8) | hibit;
        const uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16; bytes -= 16;
    }
    p.h[0] = h0; p.h[1] = h1; p.h[2] = h2; p.h[3] = h3; p.h[4] = h4;
}

void polyUpdate(Poly& p, const uint8_t* m, size_t bytes) {
    if (p.left) {
        size_t want = 16 - p.left;
        if (want > bytes) want = bytes;
        memcpy(p.buf + p.left, m, want);
        p.left += want; m += want; bytes -= want;
        if (p.left < 16) return;
        polyBlocks(p, p.buf, 16, 1u << 24);
        p.left = 0;
    }
    if (bytes >= 16) {
        const size_t want = bytes & ~(size_t)15;
        polyBlocks(p, m, want, 1u << 24);
        m += want; bytes -= want;
    }
    if (bytes) { memcpy(p.buf, m, bytes); p.left = bytes; }
}

// Pad the input so far to a 16-byte boundary with zeros (AEAD construction).
void polyPad16(Poly& p, size_t len) {
    static const uint8_t zero[16] = {0};
    if (len % 16) polyUpdate(p, zero, 16 - len % 16);
}

void polyFinish(Poly& p, uint8_t mac[16]) {
    if (p.left) {
        p.buf[p.left++] = 1;
        while (p.left < 16) p.buf[p.left++] = 0;
        polyBlocks(p, p.buf, 16, 0);
    }
    uint32_t h0 = p.h[0], h1 = p.h[1], h2 = p.h[2], h3 = p.h[3], h4 = p.h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;            // all ones if h >= p
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
    uint64_t f = (uint64_t)h0 + p.pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p.pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p.pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p.pad[3] + (f >> 32); h3 = (uint32_t)f;
    put32(mac + 0, h0); put32(mac + 4, h1); put32(mac + 8, h2); put32(mac + 12, h3);
}

void tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* ct,
         size_t len, uint8_t mac[16]) {
    uint8_t block0[64];
    chachaBlock(key, 0, nonce, block0);          // the one-time Poly1305 key
    Poly p;
    polyInit(p, block0);
    if (adLen) polyUpdate(p, ad, adLen);
    polyPad16(p, adLen);
    if (len) polyUpdate(p, ct, len);
    polyPad16(p, len);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)adLen >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i)); }
    polyUpdate(p, lens, 16);
    polyFinish(p, mac);
}

}  // namespace

void seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* in,
          size_t len, uint8_t* out) {
    chachaXor(key, 1, nonce, in, len, out);
    tag(key, nonce, ad, adLen, out, len, out + len);
}

bool open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen, const uint8_t* in,
          size_t len, uint8_t* out) {
    uint8_t mac[16];
    tag(key, nonce, ad, adLen, in, len, mac);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(mac[i] ^ in[len + i]);
    if (diff) return false;
    chachaXor(key, 1, nonce, in, len, out);
    return true;
}

}  // namespace chachapoly
