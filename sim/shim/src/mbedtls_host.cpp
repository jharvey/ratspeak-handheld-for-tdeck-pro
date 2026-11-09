// mbedtls subset for SecretVault on the host OS crypto API.
// Windows: CNG (bcrypt). Other hosts: add an implementation (e.g. OpenSSL) here.
#include "mbedtls/sim_mbedtls.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

static const mbedtls_md_info_t kSha256 = {MBEDTLS_MD_SHA256};

extern "C" {

const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t t) {
    return t == MBEDTLS_MD_SHA256 ? &kSha256 : nullptr;
}
void mbedtls_md_init(mbedtls_md_context_t* c) { if (c) { c->info = nullptr; c->hmac = 0; } }
int mbedtls_md_setup(mbedtls_md_context_t* c, const mbedtls_md_info_t* info, int hmac) {
    if (!c || !info) return -1;
    c->info = info; c->hmac = hmac;
    return 0;
}
void mbedtls_md_free(mbedtls_md_context_t* c) { if (c) c->info = nullptr; }

void mbedtls_platform_zeroize(void* buf, size_t len) { if (buf && len) SecureZeroMemory(buf, len); }

int mbedtls_sha256(const unsigned char* in, size_t n, unsigned char out[32], int is224) {
    if (is224) return -1;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return -1;
    NTSTATUS s = BCryptHash(alg, nullptr, 0, (PUCHAR)in, (ULONG)n, out, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    return NT_SUCCESS(s) ? 0 : -1;
}

int mbedtls_pkcs5_pbkdf2_hmac(mbedtls_md_context_t* ctx, const unsigned char* pw, size_t plen,
                              const unsigned char* salt, size_t slen, unsigned int iter,
                              uint32_t keyLen, unsigned char* out) {
    if (!ctx || !ctx->info || ctx->info->type != MBEDTLS_MD_SHA256) return -1;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG))) return -1;
    NTSTATUS s = BCryptDeriveKeyPBKDF2(alg, (PUCHAR)pw, (ULONG)plen, (PUCHAR)salt, (ULONG)slen,
                                       iter, out, keyLen, 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    return NT_SUCCESS(s) ? 0 : -1;
}

void mbedtls_gcm_init(mbedtls_gcm_context* c) { if (c) memset(c, 0, sizeof(*c)); }
void mbedtls_gcm_free(mbedtls_gcm_context* c) { if (c) SecureZeroMemory(c, sizeof(*c)); }
int mbedtls_gcm_setkey(mbedtls_gcm_context* c, mbedtls_cipher_id_t id, const unsigned char* key,
                       unsigned int bits) {
    if (!c || id != MBEDTLS_CIPHER_ID_AES || (bits != 128 && bits != 192 && bits != 256)) return -1;
    memcpy(c->key, key, bits / 8);
    c->keybits = bits;
    return 0;
}

static int gcmRun(mbedtls_gcm_context* c, bool enc, size_t len, const unsigned char* iv, size_t ivLen,
                  const unsigned char* aad, size_t aadLen, const unsigned char* in, unsigned char* out,
                  unsigned char* tag, size_t tagLen) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    int rc = -1;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) return -1;
    if (NT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                     sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
        NT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, c->key, c->keybits / 8, 0))) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)iv; info.cbNonce = (ULONG)ivLen;
        info.pbAuthData = (PUCHAR)aad; info.cbAuthData = (ULONG)aadLen;
        info.pbTag = tag; info.cbTag = (ULONG)tagLen;
        ULONG done = 0;
        unsigned char dummy = 0;
        PUCHAR src = len ? (PUCHAR)in : &dummy;
        PUCHAR dst = len ? out : &dummy;
        NTSTATUS s = enc ? BCryptEncrypt(key, src, (ULONG)len, &info, nullptr, 0, dst, (ULONG)len, &done, 0)
                         : BCryptDecrypt(key, src, (ULONG)len, &info, nullptr, 0, dst, (ULONG)len, &done, 0);
        rc = NT_SUCCESS(s) ? 0 : (enc ? -1 : MBEDTLS_ERR_GCM_AUTH_FAILED);
    }
    if (key) BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    return rc;
}

int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context* c, int mode, size_t len, const unsigned char* iv,
                              size_t ivLen, const unsigned char* aad, size_t aadLen,
                              const unsigned char* in, unsigned char* out, size_t tagLen, unsigned char* tag) {
    if (mode != MBEDTLS_GCM_ENCRYPT) return MBEDTLS_ERR_GCM_BAD_INPUT;
    return gcmRun(c, true, len, iv, ivLen, aad, aadLen, in, out, tag, tagLen);
}
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context* c, size_t len, const unsigned char* iv, size_t ivLen,
                             const unsigned char* aad, size_t aadLen, const unsigned char* tag,
                             size_t tagLen, const unsigned char* in, unsigned char* out) {
    unsigned char t[16];
    if (tagLen > sizeof(t)) return MBEDTLS_ERR_GCM_BAD_INPUT;
    memcpy(t, tag, tagLen);
    return gcmRun(c, false, len, iv, ivLen, aad, aadLen, in, out, t, tagLen);
}

}  // extern "C"
#else
#error "mbedtls_host.cpp: add a non-Windows crypto backend (e.g. OpenSSL) for this host"
#endif
