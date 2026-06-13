#ifndef TNU_CRYPTO_H
#define TNU_CRYPTO_H

#include <tnu/types.h>

#define TNU_SHA1_DIGEST_SIZE 20
#define TNU_WPA_PMK_LEN 32
#define TNU_WPA_PTK_LEN 64

void tnu_sha1(const void *data, size_t len, uint8_t out[TNU_SHA1_DIGEST_SIZE]);
void tnu_hmac_sha1(const void *key, size_t key_len, const void *data, size_t len,
                   uint8_t out[TNU_SHA1_DIGEST_SIZE]);
void tnu_pbkdf2_hmac_sha1(const char *passphrase, const uint8_t *ssid,
                          size_t ssid_len, uint32_t iterations,
                          uint8_t *out, size_t out_len);
void tnu_wpa_pmk_from_passphrase(const char *passphrase, const char *ssid,
                                 uint8_t pmk[TNU_WPA_PMK_LEN]);
void tnu_wpa_prf(const uint8_t *key, size_t key_len, const char *label,
                 const uint8_t *data, size_t data_len,
                 uint8_t *out, size_t out_len);
void tnu_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16],
                              uint8_t out[16]);

#endif
