#ifndef TNU_TLS_H
#define TNU_TLS_H

#include <stddef.h>
#include <stdint.h>

#define TNU_SHA256_DIGEST_SIZE 32
#define TNU_CHACHA20_KEY_SIZE 32
#define TNU_CHACHA20_NONCE_SIZE 12
#define TNU_POLY1305_TAG_SIZE 16

enum tnu_tls_error {
    TNU_TLS_OK = 0,
    TNU_TLS_ERR_UNSUPPORTED = -1,
    TNU_TLS_ERR_CRYPTO_MISSING = -2,
    TNU_TLS_ERR_NETWORK = -3,
    TNU_TLS_ERR_BAD_URL = -4,
    TNU_TLS_ERR_HANDSHAKE_INCOMPLETE = -5,
    TNU_TLS_ERR_CERT_UNTRUSTED = -6,
    TNU_TLS_ERR_CERT_HOSTNAME = -7,
    TNU_TLS_ERR_CERT_PARSE = -8,
};

struct tnu_tls_features {
    int sha256;
    int hkdf_sha256;
    int x25519;
    int aes_128_gcm;
    int chacha20;
    int poly1305;
    int chacha20_poly1305;
    int tls_record_crypto;
    int tls13_client_hello;
    int x509;
    int ca_store;
};

#define TNU_X509_NAME_MAX 128

struct tnu_x509_info {
    char common_name[TNU_X509_NAME_MAX];
    char dns_names[4][TNU_X509_NAME_MAX];
    size_t dns_name_count;
    uint8_t fingerprint_sha256[TNU_SHA256_DIGEST_SIZE];
};

typedef int (*tnu_tls_write_cb)(const void *data, size_t len, void *ctx);

const struct tnu_tls_features *tnu_tls_features(void);
const char *tnu_tls_strerror(int error);
int tnu_tls_selftest(void);
int tnu_https_get(const char *url, tnu_tls_write_cb write_cb, void *ctx);
void tnu_sha256(const void *data, size_t len,
                uint8_t out[TNU_SHA256_DIGEST_SIZE]);
void tnu_hmac_sha256(const void *key, size_t key_len,
                     const void *data, size_t len,
                     uint8_t out[TNU_SHA256_DIGEST_SIZE]);
int tnu_hkdf_sha256(const void *salt, size_t salt_len,
                    const void *ikm, size_t ikm_len,
                    const void *info, size_t info_len,
                    uint8_t *out, size_t out_len);
void tnu_chacha20_xor(const uint8_t key[TNU_CHACHA20_KEY_SIZE],
                      const uint8_t nonce[TNU_CHACHA20_NONCE_SIZE],
                      uint32_t counter, const uint8_t *in,
                      uint8_t *out, size_t len);
int tnu_x25519(uint8_t out[32], const uint8_t scalar[32],
               const uint8_t point[32]);
void tnu_poly1305_mac(const uint8_t key[32], const uint8_t *msg,
                      size_t len, uint8_t tag[TNU_POLY1305_TAG_SIZE]);
int tnu_chacha20_poly1305_seal(const uint8_t key[TNU_CHACHA20_KEY_SIZE],
                               const uint8_t nonce[TNU_CHACHA20_NONCE_SIZE],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *plain, size_t plain_len,
                               uint8_t *cipher,
                               uint8_t tag[TNU_POLY1305_TAG_SIZE]);
int tnu_chacha20_poly1305_open(const uint8_t key[TNU_CHACHA20_KEY_SIZE],
                               const uint8_t nonce[TNU_CHACHA20_NONCE_SIZE],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *cipher, size_t cipher_len,
                               const uint8_t tag[TNU_POLY1305_TAG_SIZE],
                               uint8_t *plain);
int tnu_x509_parse_der(const uint8_t *der, size_t der_len,
                       struct tnu_x509_info *out);
int tnu_x509_validate_der(const uint8_t *der, size_t der_len,
                          const char *hostname);

#endif
