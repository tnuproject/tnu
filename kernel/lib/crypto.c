/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SHA-1 core adapted for TNU from FreeBSD sys/crypto/sha1.c, itself
 * copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * HMAC/PBKDF2/WPA-PRF glue is original TNU code built on that primitive.
 */

#include <tnu/crypto.h>
#include <tnu/string.h>

struct sha1_ctx {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
};

static uint32_t rol32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void sha1_transform(struct sha1_ctx *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    for (size_t i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }
    for (size_t i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0];
    uint32_t b = ctx->h[1];
    uint32_t c = ctx->h[2];
    uint32_t d = ctx->h[3];
    uint32_t e = ctx->h[4];

    for (size_t i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = t;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->h[0] = 0x67452301;
    ctx->h[1] = 0xefcdab89;
    ctx->h[2] = 0x98badcfe;
    ctx->h[3] = 0x10325476;
    ctx->h[4] = 0xc3d2e1f0;
}

static void sha1_update(struct sha1_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    ctx->bytes += len;
    while (len) {
        size_t n = sizeof(ctx->block) - ctx->used;
        if (n > len) {
            n = len;
        }
        memcpy(ctx->block + ctx->used, p, n);
        ctx->used += n;
        p += n;
        len -= n;
        if (ctx->used == sizeof(ctx->block)) {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha1_final(struct sha1_ctx *ctx, uint8_t out[TNU_SHA1_DIGEST_SIZE])
{
    uint64_t bits = ctx->bytes * 8;
    uint8_t one = 0x80;
    uint8_t zero = 0;
    sha1_update(ctx, &one, 1);
    while (ctx->used != 56) {
        sha1_update(ctx, &zero, 1);
    }
    uint8_t lenbuf[8];
    store_be64(lenbuf, bits);
    sha1_update(ctx, lenbuf, sizeof(lenbuf));
    for (size_t i = 0; i < 5; i++) {
        store_be32(out + i * 4, ctx->h[i]);
    }
}

void tnu_sha1(const void *data, size_t len, uint8_t out[TNU_SHA1_DIGEST_SIZE])
{
    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, out);
}

void tnu_hmac_sha1(const void *key, size_t key_len, const void *data, size_t len,
                   uint8_t out[TNU_SHA1_DIGEST_SIZE])
{
    uint8_t k0[64];
    uint8_t digest[TNU_SHA1_DIGEST_SIZE];
    memset(k0, 0, sizeof(k0));
    if (key_len > sizeof(k0)) {
        tnu_sha1(key, key_len, digest);
        memcpy(k0, digest, sizeof(digest));
    } else if (key_len) {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < sizeof(k0); i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }

    struct sha1_ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, ipad, sizeof(ipad));
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);

    sha1_init(&ctx);
    sha1_update(&ctx, opad, sizeof(opad));
    sha1_update(&ctx, digest, sizeof(digest));
    sha1_final(&ctx, out);
}

void tnu_pbkdf2_hmac_sha1(const char *passphrase, const uint8_t *ssid,
                          size_t ssid_len, uint32_t iterations,
                          uint8_t *out, size_t out_len)
{
    size_t pass_len = strlen(passphrase);
    uint8_t salt_block[36];
    uint8_t u[TNU_SHA1_DIGEST_SIZE];
    uint8_t t[TNU_SHA1_DIGEST_SIZE];
    uint32_t block = 1;
    size_t done = 0;

    while (done < out_len) {
        memcpy(salt_block, ssid, ssid_len);
        salt_block[ssid_len + 0] = (uint8_t)(block >> 24);
        salt_block[ssid_len + 1] = (uint8_t)(block >> 16);
        salt_block[ssid_len + 2] = (uint8_t)(block >> 8);
        salt_block[ssid_len + 3] = (uint8_t)block;
        tnu_hmac_sha1(passphrase, pass_len, salt_block, ssid_len + 4, u);
        memcpy(t, u, sizeof(t));
        for (uint32_t i = 1; i < iterations; i++) {
            tnu_hmac_sha1(passphrase, pass_len, u, sizeof(u), u);
            for (size_t j = 0; j < sizeof(t); j++) {
                t[j] ^= u[j];
            }
        }
        size_t take = out_len - done;
        if (take > sizeof(t)) {
            take = sizeof(t);
        }
        memcpy(out + done, t, take);
        done += take;
        block++;
    }
}

void tnu_wpa_pmk_from_passphrase(const char *passphrase, const char *ssid,
                                 uint8_t pmk[TNU_WPA_PMK_LEN])
{
    tnu_pbkdf2_hmac_sha1(passphrase, (const uint8_t *)ssid, strlen(ssid),
                         4096, pmk, TNU_WPA_PMK_LEN);
}

void tnu_wpa_prf(const uint8_t *key, size_t key_len, const char *label,
                 const uint8_t *data, size_t data_len,
                 uint8_t *out, size_t out_len)
{
    uint8_t input[128];
    size_t label_len = strlen(label);
    size_t base_len = label_len + 1 + data_len + 1;
    if (base_len > sizeof(input)) {
        return;
    }
    memcpy(input, label, label_len);
    input[label_len] = 0;
    memcpy(input + label_len + 1, data, data_len);

    size_t done = 0;
    uint8_t counter = 0;
    while (done < out_len) {
        input[base_len - 1] = counter++;
        uint8_t digest[TNU_SHA1_DIGEST_SIZE];
        tnu_hmac_sha1(key, key_len, input, base_len, digest);
        size_t take = out_len - done;
        if (take > sizeof(digest)) {
            take = sizeof(digest);
        }
        memcpy(out + done, digest, take);
        done += take;
    }
}
