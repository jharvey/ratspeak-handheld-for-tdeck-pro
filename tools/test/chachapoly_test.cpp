// RFC 8439 section 2.8.2 test vector for src/boards/tdeck_Pro/voice/ChaChaPoly.cpp.
// g++ -O2 -I src/boards/tdeck_Pro/voice tools/test/chachapoly_test.cpp src/boards/tdeck_Pro/voice/ChaChaPoly.cpp && ./a.out
#include <stdio.h>
#include <string.h>

#include "ChaChaPoly.h"

int main() {
    uint8_t key[32], nonce[12] = {0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    const uint8_t ad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    const char* pt =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const size_t n = strlen(pt);
    const uint8_t expectStart[16] = {0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
                                     0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2};
    const uint8_t expectEnd[2] = {0x61, 0x16};
    const uint8_t expectTag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
                                   0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
    uint8_t out[256], back[256];
    chachapoly::seal(key, nonce, ad, sizeof(ad), (const uint8_t*)pt, n, out);
    int bad = 0;
    if (memcmp(out, expectStart, 16)) { puts("FAIL ciphertext start"); bad++; }
    if (memcmp(out + n - 2, expectEnd, 2)) { puts("FAIL ciphertext end"); bad++; }
    if (memcmp(out + n, expectTag, 16)) { puts("FAIL tag"); bad++; }
    if (!chachapoly::open(key, nonce, ad, sizeof(ad), out, n, back) || memcmp(back, pt, n)) { puts("FAIL open"); bad++; }
    out[5] ^= 1;
    if (chachapoly::open(key, nonce, ad, sizeof(ad), out, n, back)) { puts("FAIL tamper accepted"); bad++; }
    // Empty plaintext (Noise handshake payloads): tag only, must round-trip.
    uint8_t t[16];
    chachapoly::seal(key, nonce, ad, sizeof(ad), nullptr, 0, t);
    if (!chachapoly::open(key, nonce, ad, sizeof(ad), t, 0, back)) { puts("FAIL empty"); bad++; }
    // Odd lengths across block / 16-byte boundaries round-trip.
    for (size_t len = 0; len < 200; len++) {
        uint8_t m[200];
        for (size_t i = 0; i < len; i++) m[i] = (uint8_t)(i * 7 + len);
        chachapoly::seal(key, nonce, nullptr, 0, m, len, out);
        if (!chachapoly::open(key, nonce, nullptr, 0, out, len, back) || memcmp(back, m, len)) {
            printf("FAIL len %zu\n", len);
            bad++;
            break;
        }
    }
    puts(bad ? "chachapoly: FAILED" : "chachapoly: RFC 8439 vector OK");
    return bad;
}
