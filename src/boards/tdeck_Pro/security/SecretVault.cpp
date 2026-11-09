#include "security/SecretVault.h"

#include <Preferences.h>
#include <esp_system.h>
#include <string.h>
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

namespace vault {
namespace {

// Built-in password used until the user sets one. It is public (open source);
// its value comes from being combined with the per-device salt + chip MAC.
constexpr const char* kDefaultPassword = "ratspeak-tdeck-pro-default-v1";
constexpr const char* kNs = "rs_vault";
constexpr uint32_t kIterations = 10000;   // ~0.2-0.5 s on the S3 with HW SHA
constexpr size_t kSaltLen = 16, kKeyLen = 32, kNonceLen = 12, kTagLen = 16;

uint8_t  g_key[kKeyLen];
bool     g_unlocked = false;
bool     g_custom = false;
bool     g_ready = false;
uint8_t  g_salt[kSaltLen];
uint8_t  g_verifier[32];
uint32_t g_iter = kIterations;

// out = key[32] | verifier seed[32]
bool derive(const char* pw, const uint8_t* salt, uint32_t iter, uint8_t out[64]) {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    const size_t pwLen = strlen(pw);
    const size_t inLen = pwLen + 1 + sizeof(mac);
    uint8_t* in = (uint8_t*)malloc(inLen);
    if (!in) return false;
    memcpy(in, pw, pwLen);
    in[pwLen] = 0;                          // separator
    memcpy(in + pwLen + 1, mac, sizeof(mac));

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    if (rc == 0) rc = mbedtls_pkcs5_pbkdf2_hmac(&md, in, inLen, salt, kSaltLen, iter, 64, out);
    mbedtls_md_free(&md);
    mbedtls_platform_zeroize(in, inLen);
    free(in);
    return rc == 0;
}

void verifierOf(const uint8_t seed[32], uint8_t ver[32]) {
    mbedtls_sha256(seed, 32, ver, 0);
}

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

bool tryPassword(const char* pw, bool keepKey) {
    uint8_t dk[64], ver[32];
    if (!derive(pw, g_salt, g_iter, dk)) return false;
    verifierOf(dk + 32, ver);
    const bool ok = ctEqual(ver, g_verifier, sizeof(ver));
    if (ok && keepKey) { memcpy(g_key, dk, kKeyLen); g_unlocked = true; }
    mbedtls_platform_zeroize(dk, sizeof(dk));
    return ok;
}

void toHex(const uint8_t* b, size_t n, String& out) {
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out += hx[b[i] >> 4]; out += hx[b[i] & 15]; }
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool fromHex(const char* s, size_t len, uint8_t* out) {
    for (size_t i = 0; i < len / 2; i++) {
        const int h = hexVal(s[2 * i]), l = hexVal(s[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

}  // namespace

bool setPassword(const char* pw) {
    const bool custom = pw && *pw;
    const char* use = custom ? pw : kDefaultPassword;
    uint8_t salt[kSaltLen], dk[64], ver[32];
    esp_fill_random(salt, sizeof(salt));
    if (!derive(use, salt, kIterations, dk)) return false;
    verifierOf(dk + 32, ver);

    Preferences p;
    if (!p.begin(kNs, false)) { mbedtls_platform_zeroize(dk, sizeof(dk)); return false; }
    p.putBytes("salt", salt, sizeof(salt));
    p.putBytes("ver", ver, sizeof(ver));
    p.putUInt("it", kIterations);
    p.putUChar("custom", custom ? 1 : 0);
    p.end();

    memcpy(g_salt, salt, sizeof(salt));
    memcpy(g_verifier, ver, sizeof(ver));
    memcpy(g_key, dk, kKeyLen);
    mbedtls_platform_zeroize(dk, sizeof(dk));
    g_iter = kIterations;
    g_custom = custom;
    g_unlocked = true;
    g_ready = true;
    Serial.printf("[VAULT] new %s key\n", custom ? "custom" : "default");
    return true;
}

bool begin() {
    const uint32_t t0 = millis();
    Preferences p;
    bool have = false;
    if (p.begin(kNs, true)) {
        have = p.getBytes("salt", g_salt, kSaltLen) == kSaltLen &&
               p.getBytes("ver", g_verifier, sizeof(g_verifier)) == sizeof(g_verifier);
        g_iter = p.getUInt("it", kIterations);
        g_custom = p.getUChar("custom", 0) != 0;
        p.end();
    }
    if (!have) {
        if (!setPassword(nullptr)) { Serial.println("[VAULT] init failed"); return false; }
    } else if (g_custom) {
        g_ready = true;                       // waits for unlock(pw)
        Serial.println("[VAULT] custom password set: locked until unlock");
        return true;
    } else if (!tryPassword(kDefaultPassword, true)) {
        // Default changed or vault data damaged: old secrets are unreadable either way.
        Serial.println("[VAULT] default key mismatch, re-initialising (saved secrets lost)");
        if (!setPassword(nullptr)) return false;
    }
    g_ready = true;
    Serial.printf("[VAULT] unlocked (default password) in %u ms\n", (unsigned)(millis() - t0));
    return true;
}

bool unlocked() { return g_unlocked; }
bool usingDefaultPassword() { return !g_custom; }

bool checkPassword(const char* pw) {
    if (!g_ready || !pw) return false;
    return tryPassword(pw, false);
}

bool unlock(const char* pw) {
    if (!g_ready || !pw) return false;
    if (g_unlocked) return checkPassword(pw);
    return tryPassword(pw, true);
}

void lock() {
    if (!g_custom) return;                    // the default key would just re-derive
    mbedtls_platform_zeroize(g_key, sizeof(g_key));
    g_unlocked = false;
}

bool isSealed(const char* stored) {
    return stored && strncmp(stored, "v1:", 3) == 0;
}

bool seal(const char* plain, const char* aad, String& out) {
    if (!g_unlocked || !plain) return false;
    const size_t n = strlen(plain);
    uint8_t nonce[kNonceLen], tag[kTagLen];
    uint8_t* ct = (uint8_t*)malloc(n ? n : 1);
    if (!ct) return false;
    esp_fill_random(nonce, sizeof(nonce));
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, g_key, kKeyLen * 8);
    const size_t aadLen = aad ? strlen(aad) : 0;
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, nonce, sizeof(nonce),
                                       (const uint8_t*)aad, aadLen,
                                       (const uint8_t*)plain, ct, sizeof(tag), tag);
    mbedtls_gcm_free(&g);
    if (rc == 0) {
        out = "v1:";
        out.reserve(3 + 2 * (kNonceLen + n + kTagLen));
        toHex(nonce, sizeof(nonce), out);
        toHex(ct, n, out);
        toHex(tag, sizeof(tag), out);
    }
    free(ct);
    return rc == 0;
}

bool unseal(const char* stored, const char* aad, char* out, size_t outCap) {
    if (!g_unlocked || !isSealed(stored) || !out || !outCap) return false;
    const char* hex = stored + 3;
    const size_t hexLen = strlen(hex);
    if (hexLen % 2 || hexLen < 2 * (kNonceLen + kTagLen)) return false;
    const size_t total = hexLen / 2, n = total - kNonceLen - kTagLen;
    if (n + 1 > outCap) return false;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return false;
    bool ok = fromHex(hex, hexLen, buf);
    if (ok) {
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, g_key, kKeyLen * 8);
        const size_t aadLen = aad ? strlen(aad) : 0;
        if (rc == 0)
            rc = mbedtls_gcm_auth_decrypt(&g, n, buf, kNonceLen, (const uint8_t*)aad, aadLen,
                                          buf + kNonceLen + n, kTagLen,
                                          buf + kNonceLen, (uint8_t*)out);
        mbedtls_gcm_free(&g);
        ok = rc == 0;
    }
    if (ok) out[n] = 0;
    else    mbedtls_platform_zeroize(out, outCap);
    mbedtls_platform_zeroize(buf, total);
    free(buf);
    return ok;
}

const char* status() {
    if (!g_ready)    return "unavailable";
    if (!g_unlocked) return "locked";
    return g_custom ? "custom password" : "default password";
}

}  // namespace vault
