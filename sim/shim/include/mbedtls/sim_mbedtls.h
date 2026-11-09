#pragma once
// The mbedtls subset used by SecretVault, implemented on the host OS crypto API
// (Windows CNG / bcrypt) in sim/shim/src/mbedtls_host.cpp. Same signatures as
// mbedtls 2.28, so the firmware code compiles unchanged.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
typedef struct mbedtls_md_info_t { mbedtls_md_type_t type; } mbedtls_md_info_t;
typedef struct mbedtls_md_context_t { const mbedtls_md_info_t* info; int hmac; } mbedtls_md_context_t;

typedef enum { MBEDTLS_CIPHER_ID_NONE = 0, MBEDTLS_CIPHER_ID_AES = 2 } mbedtls_cipher_id_t;
#define MBEDTLS_GCM_ENCRYPT 1
#define MBEDTLS_GCM_DECRYPT 0
#define MBEDTLS_ERR_GCM_AUTH_FAILED (-0x0012)
#define MBEDTLS_ERR_GCM_BAD_INPUT (-0x0014)
typedef struct mbedtls_gcm_context { unsigned char key[32]; unsigned keybits; } mbedtls_gcm_context;

const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t);
void mbedtls_md_init(mbedtls_md_context_t* ctx);
int mbedtls_md_setup(mbedtls_md_context_t* ctx, const mbedtls_md_info_t* info, int hmac);
void mbedtls_md_free(mbedtls_md_context_t* ctx);

int mbedtls_pkcs5_pbkdf2_hmac(mbedtls_md_context_t* ctx, const unsigned char* password, size_t plen,
                              const unsigned char* salt, size_t slen, unsigned int iterations,
                              uint32_t key_length, unsigned char* output);

int mbedtls_sha256(const unsigned char* input, size_t ilen, unsigned char output[32], int is224);
void mbedtls_platform_zeroize(void* buf, size_t len);

void mbedtls_gcm_init(mbedtls_gcm_context* ctx);
int mbedtls_gcm_setkey(mbedtls_gcm_context* ctx, mbedtls_cipher_id_t cipher, const unsigned char* key,
                       unsigned int keybits);
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context* ctx, int mode, size_t length, const unsigned char* iv,
                              size_t iv_len, const unsigned char* add, size_t add_len,
                              const unsigned char* input, unsigned char* output, size_t tag_len,
                              unsigned char* tag);
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context* ctx, size_t length, const unsigned char* iv,
                             size_t iv_len, const unsigned char* add, size_t add_len,
                             const unsigned char* tag, size_t tag_len, const unsigned char* input,
                             unsigned char* output);
void mbedtls_gcm_free(mbedtls_gcm_context* ctx);

#ifdef __cplusplus
}
#endif
