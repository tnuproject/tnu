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

static uint8_t aes_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static uint8_t aes_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1) {
            r ^= a;
        }
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

static uint8_t aes_pow(uint8_t a, uint8_t n)
{
    uint8_t r = 1;
    while (n) {
        if (n & 1) {
            r = aes_mul(r, a);
        }
        a = aes_mul(a, a);
        n >>= 1;
    }
    return r;
}

static uint8_t aes_sbox(uint8_t x)
{
    uint8_t y = x ? aes_pow(x, 254) : 0;
    uint8_t s = (uint8_t)(0x63 ^ y ^ ((y << 1) | (y >> 7)) ^
                          ((y << 2) | (y >> 6)) ^
                          ((y << 3) | (y >> 5)) ^
                          ((y << 4) | (y >> 4)));
    return s;
}

static void aes_sub_bytes(uint8_t s[16])
{
    for (size_t i = 0; i < 16; i++) {
        s[i] = aes_sbox(s[i]);
    }
}

static void aes_shift_rows(uint8_t s[16])
{
    uint8_t t[16];
    t[0] = s[0];   t[1] = s[5];   t[2] = s[10];  t[3] = s[15];
    t[4] = s[4];   t[5] = s[9];   t[6] = s[14];  t[7] = s[3];
    t[8] = s[8];   t[9] = s[13];  t[10] = s[2];  t[11] = s[7];
    t[12] = s[12]; t[13] = s[1];  t[14] = s[6];  t[15] = s[11];
    memcpy(s, t, sizeof(t));
}

static void aes_mix_columns(uint8_t s[16])
{
    for (size_t c = 0; c < 4; c++) {
        uint8_t *p = s + c * 4;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = aes_mul(a0, 2) ^ aes_mul(a1, 3) ^ a2 ^ a3;
        p[1] = a0 ^ aes_mul(a1, 2) ^ aes_mul(a2, 3) ^ a3;
        p[2] = a0 ^ a1 ^ aes_mul(a2, 2) ^ aes_mul(a3, 3);
        p[3] = aes_mul(a0, 3) ^ a1 ^ a2 ^ aes_mul(a3, 2);
    }
}

static void aes_add_round_key(uint8_t s[16], const uint8_t *rk)
{
    for (size_t i = 0; i < 16; i++) {
        s[i] ^= rk[i];
    }
}

static void aes_expand_key(const uint8_t key[16], uint8_t round_keys[176])
{
    static const uint8_t rcon[10] =
        { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };
    memcpy(round_keys, key, 16);
    size_t bytes = 16;
    size_t rcon_index = 0;
    uint8_t temp[4];
    while (bytes < 176) {
        memcpy(temp, round_keys + bytes - 4, 4);
        if ((bytes % 16) == 0) {
            uint8_t first = temp[0];
            temp[0] = aes_sbox(temp[1]) ^ rcon[rcon_index++];
            temp[1] = aes_sbox(temp[2]);
            temp[2] = aes_sbox(temp[3]);
            temp[3] = aes_sbox(first);
        }
        for (size_t i = 0; i < 4; i++) {
            round_keys[bytes] = round_keys[bytes - 16] ^ temp[i];
            bytes++;
        }
    }
}

void tnu_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16],
                              uint8_t out[16])
{
    uint8_t round_keys[176];
    uint8_t state[16];
    aes_expand_key(key, round_keys);
    memcpy(state, in, sizeof(state));

    aes_add_round_key(state, round_keys);
    for (size_t round = 1; round < 10; round++) {
        aes_sub_bytes(state);
        aes_shift_rows(state);
        aes_mix_columns(state);
        aes_add_round_key(state, round_keys + round * 16);
    }
    aes_sub_bytes(state);
    aes_shift_rows(state);
    aes_add_round_key(state, round_keys + 160);
    memcpy(out, state, 16);
}
