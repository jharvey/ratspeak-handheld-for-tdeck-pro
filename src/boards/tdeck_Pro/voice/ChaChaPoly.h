#pragma once
// ChaCha20-Poly1305 AEAD (RFC 8439), for the ESPHome native API's Noise encryption.
// The framework's mbedTLS is built without CHACHAPOLY, so this is a small portable copy
// (checked against the RFC 8439 test vectors: tools/test/chachapoly_test.cpp).
#include <stddef.h>
#include <stdint.h>

namespace chachapoly {

// out = ciphertext (len bytes) followed by the 16-byte tag.
void seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen,
          const uint8_t* in, size_t len, uint8_t* out);
// in = ciphertext (len bytes) + 16-byte tag. false: tag mismatch (out untouched past len).
bool open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* ad, size_t adLen,
          const uint8_t* in, size_t len, uint8_t* out);

// Noise nonce: 4 zero bytes then the 64-bit counter, little-endian.
inline void noiseNonce(uint64_t n, uint8_t out[12]) {
    for (int i = 0; i < 4; i++) out[i] = 0;
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(n >> (8 * i));
}

}  // namespace chachapoly
