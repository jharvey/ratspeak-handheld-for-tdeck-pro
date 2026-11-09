#pragma once
// SecretVault: encrypts small secrets (WiFi passwords, later others) at rest in NVS.
//
// Key = PBKDF2-HMAC-SHA256(password || chip MAC, random per-device salt).
// Secrets are AES-256-GCM sealed with a random nonce and an AAD label, stored as
// "v1:<hex nonce|ciphertext|tag>". A verifier derived from the same password
// lets checkPassword() test a password without decrypting anything.
//
// Today the password is a built-in default that is always applied automatically
// at boot (no prompt). Because the salt and chip MAC are mixed in, a flash dump
// cannot be decrypted from the public source alone; but someone holding the
// device itself can still run the firmware, so real protection starts once the
// user sets their own password (idle-lock password, planned).
//
// Planned user-password flow:
//   begin()                 -> locked() if a custom password was set
//   unlock(pw)              -> derives the key if the verifier matches
//   decrypt every secret    -> setPassword(newPw) -> re-encrypt + save every secret
//   clearCustomPassword()   -> back to the built-in default (same re-encrypt dance)

#include <Arduino.h>

namespace vault {

bool begin();                       // load/create salt; auto-unlock with the default password
bool unlocked();
bool usingDefaultPassword();
bool checkPassword(const char* pw); // verifier match (constant time)
bool unlock(const char* pw);        // derive the key for a custom password
void lock();                        // forget the key in RAM

// Replace the password (custom when pw != nullptr, built-in default otherwise).
// New salt + verifier; the caller must re-encrypt every secret afterwards.
bool setPassword(const char* pw);

bool isSealed(const char* stored);  // "v1:..." ?
// aad binds the ciphertext to what it belongs to (e.g. the SSID).
bool seal(const char* plain, const char* aad, String& out);
bool unseal(const char* stored, const char* aad, char* out, size_t outCap);

const char* status();               // short text for the settings UI

}  // namespace vault
