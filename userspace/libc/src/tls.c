#include <tnu/tls.h>
#include <tnu/syscall.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

struct tls_url {
    char host[128];
    char path[256];
    uint16_t port;
};

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t len);

static const struct tnu_tls_features tls_features = {
    .sha256 = 1,
    .hkdf_sha256 = 1,
    .x25519 = 1,
    .aes_128_gcm = 0,
    .chacha20 = 1,
    .poly1305 = 1,
    .chacha20_poly1305 = 1,
    .tls_record_crypto = 1,
    .tls13_client_hello = 1,
    .x509 = 1,
    .ca_store = 1,
};

struct sha256_ctx {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
};

static uint32_t ror32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t load_be32_tls(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void store_be16_tls(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void store_be24_tls(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static void store_be32_tls(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64_tls(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void sha256_init(struct sha256_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
}

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = load_be32_tls(block + i * 4);
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len)
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
            sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t out[TNU_SHA256_DIGEST_SIZE])
{
    uint64_t bits = ctx->bytes * 8;
    uint8_t one = 0x80;
    uint8_t zero = 0;
    sha256_update(ctx, &one, 1);
    while (ctx->used != 56) {
        sha256_update(ctx, &zero, 1);
    }
    uint8_t lenbuf[8];
    store_be64_tls(lenbuf, bits);
    sha256_update(ctx, lenbuf, sizeof(lenbuf));
    for (size_t i = 0; i < 8; i++) {
        store_be32_tls(out + i * 4, ctx->h[i]);
    }
}

void tnu_sha256(const void *data, size_t len, uint8_t out[TNU_SHA256_DIGEST_SIZE])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void tnu_hmac_sha256(const void *key, size_t key_len,
                     const void *data, size_t len,
                     uint8_t out[TNU_SHA256_DIGEST_SIZE])
{
    uint8_t k0[64];
    uint8_t digest[TNU_SHA256_DIGEST_SIZE];
    memset(k0, 0, sizeof(k0));
    if (key_len > sizeof(k0)) {
        tnu_sha256(key, key_len, digest);
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

    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, digest, sizeof(digest));
    sha256_final(&ctx, out);
}

int tnu_hkdf_sha256(const void *salt, size_t salt_len,
                    const void *ikm, size_t ikm_len,
                    const void *info, size_t info_len,
                    uint8_t *out, size_t out_len)
{
    if (!out && out_len) {
        return TNU_TLS_ERR_BAD_URL;
    }
    if (out_len > 255u * TNU_SHA256_DIGEST_SIZE) {
        return TNU_TLS_ERR_UNSUPPORTED;
    }
    uint8_t zero_salt[TNU_SHA256_DIGEST_SIZE];
    uint8_t prk[TNU_SHA256_DIGEST_SIZE];
    uint8_t t[TNU_SHA256_DIGEST_SIZE];
    size_t t_len = 0;
    size_t done = 0;
    uint8_t counter = 1;

    if (!salt) {
        memset(zero_salt, 0, sizeof(zero_salt));
        salt = zero_salt;
        salt_len = sizeof(zero_salt);
    }
    tnu_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);

    while (done < out_len) {
        struct sha256_ctx ctx;
        uint8_t k0[64];
        uint8_t digest[TNU_SHA256_DIGEST_SIZE];
        memset(k0, 0, sizeof(k0));
        memcpy(k0, prk, sizeof(prk));
        uint8_t ipad[64];
        uint8_t opad[64];
        for (size_t i = 0; i < sizeof(k0); i++) {
            ipad[i] = k0[i] ^ 0x36;
            opad[i] = k0[i] ^ 0x5c;
        }
        sha256_init(&ctx);
        sha256_update(&ctx, ipad, sizeof(ipad));
        if (t_len) {
            sha256_update(&ctx, t, t_len);
        }
        if (info_len) {
            sha256_update(&ctx, info, info_len);
        }
        sha256_update(&ctx, &counter, 1);
        sha256_final(&ctx, digest);

        sha256_init(&ctx);
        sha256_update(&ctx, opad, sizeof(opad));
        sha256_update(&ctx, digest, sizeof(digest));
        sha256_final(&ctx, t);
        t_len = sizeof(t);

        size_t take = out_len - done;
        if (take > t_len) {
            take = t_len;
        }
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    return TNU_TLS_OK;
}

static uint32_t load_le32_tls(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32_tls(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void chacha_qr(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b; *d ^= *a; *d = (*d << 16) | (*d >> 16);
    *c += *d; *b ^= *c; *b = (*b << 12) | (*b >> 20);
    *a += *b; *d ^= *a; *d = (*d << 8) | (*d >> 24);
    *c += *d; *b ^= *c; *b = (*b << 7) | (*b >> 25);
}

static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                           uint32_t counter, uint8_t out[64])
{
    static const uint8_t sigma[16] = "expand 32-byte k";
    uint32_t x[16];
    x[0] = load_le32_tls(sigma + 0);
    x[1] = load_le32_tls(sigma + 4);
    x[2] = load_le32_tls(sigma + 8);
    x[3] = load_le32_tls(sigma + 12);
    for (int i = 0; i < 8; i++) {
        x[4 + i] = load_le32_tls(key + i * 4);
    }
    x[12] = counter;
    x[13] = load_le32_tls(nonce + 0);
    x[14] = load_le32_tls(nonce + 4);
    x[15] = load_le32_tls(nonce + 8);

    uint32_t z[16];
    memcpy(z, x, sizeof(z));
    for (int i = 0; i < 10; i++) {
        chacha_qr(&z[0], &z[4], &z[8], &z[12]);
        chacha_qr(&z[1], &z[5], &z[9], &z[13]);
        chacha_qr(&z[2], &z[6], &z[10], &z[14]);
        chacha_qr(&z[3], &z[7], &z[11], &z[15]);
        chacha_qr(&z[0], &z[5], &z[10], &z[15]);
        chacha_qr(&z[1], &z[6], &z[11], &z[12]);
        chacha_qr(&z[2], &z[7], &z[8], &z[13]);
        chacha_qr(&z[3], &z[4], &z[9], &z[14]);
    }
    for (int i = 0; i < 16; i++) {
        store_le32_tls(out + i * 4, z[i] + x[i]);
    }
}

void tnu_chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                      uint32_t counter, const uint8_t *in,
                      uint8_t *out, size_t len)
{
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(key, nonce, counter++, block);
        size_t take = len - off;
        if (take > sizeof(block)) {
            take = sizeof(block);
        }
        for (size_t i = 0; i < take; i++) {
            out[off + i] = in[off + i] ^ block[i];
        }
        off += take;
    }
}

static uint64_t load_le64_tls(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void store_le64_tls(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

#define FE_MASK ((uint64_t)((1ULL << 51) - 1))

struct fe25519 {
    uint64_t v[5];
};

static void fe_normalize(struct fe25519 *f)
{
    uint64_t c;
    c = f->v[0] >> 51; f->v[0] &= FE_MASK; f->v[1] += c;
    c = f->v[1] >> 51; f->v[1] &= FE_MASK; f->v[2] += c;
    c = f->v[2] >> 51; f->v[2] &= FE_MASK; f->v[3] += c;
    c = f->v[3] >> 51; f->v[3] &= FE_MASK; f->v[4] += c;
    c = f->v[4] >> 51; f->v[4] &= FE_MASK; f->v[0] += c * 19;
    c = f->v[0] >> 51; f->v[0] &= FE_MASK; f->v[1] += c;
}

static void fe_from_bytes(struct fe25519 *f, const uint8_t in[32])
{
    f->v[0] = load_le64_tls(in) & FE_MASK;
    f->v[1] = (load_le64_tls(in + 6) >> 3) & FE_MASK;
    f->v[2] = (load_le64_tls(in + 12) >> 6) & FE_MASK;
    f->v[3] = (load_le64_tls(in + 19) >> 1) & FE_MASK;
    f->v[4] = (load_le64_tls(in + 24) >> 12) & FE_MASK;
}

static void fe_to_bytes(uint8_t out[32], struct fe25519 f)
{
    fe_normalize(&f);
    struct fe25519 p = {{
        (1ULL << 51) - 19,
        (1ULL << 51) - 1,
        (1ULL << 51) - 1,
        (1ULL << 51) - 1,
        (1ULL << 51) - 1,
    }};
    struct fe25519 t = f;
    uint64_t borrow = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t sub = p.v[i] + borrow;
        borrow = t.v[i] < sub;
        t.v[i] = t.v[i] - sub;
    }
    if (!borrow) {
        f = t;
    }
    uint64_t q0 = f.v[0] | (f.v[1] << 51);
    uint64_t q1 = (f.v[1] >> 13) | (f.v[2] << 38);
    uint64_t q2 = (f.v[2] >> 26) | (f.v[3] << 25);
    uint64_t q3 = (f.v[3] >> 39) | (f.v[4] << 12);
    store_le64_tls(out, q0);
    store_le64_tls(out + 8, q1);
    store_le64_tls(out + 16, q2);
    store_le64_tls(out + 24, q3);
}

static void fe_add(struct fe25519 *out, const struct fe25519 *a, const struct fe25519 *b)
{
    for (int i = 0; i < 5; i++) {
        out->v[i] = a->v[i] + b->v[i];
    }
    fe_normalize(out);
}

static void fe_sub(struct fe25519 *out, const struct fe25519 *a, const struct fe25519 *b)
{
    static const uint64_t twice_p[5] = {
        ((1ULL << 52) - 38),
        ((1ULL << 52) - 2),
        ((1ULL << 52) - 2),
        ((1ULL << 52) - 2),
        ((1ULL << 52) - 2),
    };
    for (int i = 0; i < 5; i++) {
        out->v[i] = a->v[i] + twice_p[i] - b->v[i];
    }
    fe_normalize(out);
}

static void fe_mul(struct fe25519 *out, const struct fe25519 *a, const struct fe25519 *b)
{
    unsigned __int128 t[5];
    t[0] = (unsigned __int128)a->v[0] * b->v[0] +
           (unsigned __int128)19 * (a->v[1] * (unsigned __int128)b->v[4] +
                                    a->v[2] * (unsigned __int128)b->v[3] +
                                    a->v[3] * (unsigned __int128)b->v[2] +
                                    a->v[4] * (unsigned __int128)b->v[1]);
    t[1] = (unsigned __int128)a->v[0] * b->v[1] +
           (unsigned __int128)a->v[1] * b->v[0] +
           (unsigned __int128)19 * (a->v[2] * (unsigned __int128)b->v[4] +
                                    a->v[3] * (unsigned __int128)b->v[3] +
                                    a->v[4] * (unsigned __int128)b->v[2]);
    t[2] = (unsigned __int128)a->v[0] * b->v[2] +
           (unsigned __int128)a->v[1] * b->v[1] +
           (unsigned __int128)a->v[2] * b->v[0] +
           (unsigned __int128)19 * (a->v[3] * (unsigned __int128)b->v[4] +
                                    a->v[4] * (unsigned __int128)b->v[3]);
    t[3] = (unsigned __int128)a->v[0] * b->v[3] +
           (unsigned __int128)a->v[1] * b->v[2] +
           (unsigned __int128)a->v[2] * b->v[1] +
           (unsigned __int128)a->v[3] * b->v[0] +
           (unsigned __int128)19 * a->v[4] * b->v[4];
    t[4] = (unsigned __int128)a->v[0] * b->v[4] +
           (unsigned __int128)a->v[1] * b->v[3] +
           (unsigned __int128)a->v[2] * b->v[2] +
           (unsigned __int128)a->v[3] * b->v[1] +
           (unsigned __int128)a->v[4] * b->v[0];

    uint64_t r[5];
    uint64_t c;
    r[0] = (uint64_t)t[0] & FE_MASK; t[1] += t[0] >> 51;
    r[1] = (uint64_t)t[1] & FE_MASK; t[2] += t[1] >> 51;
    r[2] = (uint64_t)t[2] & FE_MASK; t[3] += t[2] >> 51;
    r[3] = (uint64_t)t[3] & FE_MASK; t[4] += t[3] >> 51;
    r[4] = (uint64_t)t[4] & FE_MASK; c = (uint64_t)(t[4] >> 51);
    r[0] += c * 19;
    out->v[0] = r[0]; out->v[1] = r[1]; out->v[2] = r[2];
    out->v[3] = r[3]; out->v[4] = r[4];
    fe_normalize(out);
}

static void fe_square(struct fe25519 *out, const struct fe25519 *a)
{
    fe_mul(out, a, a);
}

static void fe_mul_small(struct fe25519 *out, const struct fe25519 *a, uint64_t n)
{
    for (int i = 0; i < 5; i++) {
        out->v[i] = a->v[i] * n;
    }
    fe_normalize(out);
}

static void fe_cswap(struct fe25519 *a, struct fe25519 *b, uint64_t swap)
{
    uint64_t mask = 0 - swap;
    for (int i = 0; i < 5; i++) {
        uint64_t x = mask & (a->v[i] ^ b->v[i]);
        a->v[i] ^= x;
        b->v[i] ^= x;
    }
}

static void fe_pow_pminus2(struct fe25519 *out, const struct fe25519 *z)
{
    static const uint8_t exp[32] = {
        0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    };
    struct fe25519 result = {{1, 0, 0, 0, 0}};
    struct fe25519 base = *z;
    for (int bit = 254; bit >= 0; bit--) {
        fe_square(&result, &result);
        if ((exp[bit / 8] >> (bit & 7)) & 1) {
            fe_mul(&result, &result, &base);
        }
    }
    *out = result;
}

int tnu_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    struct fe25519 x1, x2 = {{1, 0, 0, 0, 0}}, z2 = {{0, 0, 0, 0, 0}};
    struct fe25519 x3, z3 = {{1, 0, 0, 0, 0}};
    fe_from_bytes(&x1, point);
    x3 = x1;
    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint64_t k_t = (e[t / 8] >> (t & 7)) & 1;
        swap ^= k_t;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = k_t;

        struct fe25519 a, aa, b, bb, e0, c, d, da, cb, t0, t1;
        fe_add(&a, &x2, &z2);
        fe_square(&aa, &a);
        fe_sub(&b, &x2, &z2);
        fe_square(&bb, &b);
        fe_sub(&e0, &aa, &bb);
        fe_add(&c, &x3, &z3);
        fe_sub(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &b);
        fe_add(&t0, &da, &cb);
        fe_square(&x3, &t0);
        fe_sub(&t1, &da, &cb);
        fe_square(&t1, &t1);
        fe_mul(&z3, &t1, &x1);
        fe_mul(&x2, &aa, &bb);
        fe_mul_small(&t0, &e0, 121666);
        fe_add(&t0, &aa, &t0);
        fe_mul(&z2, &e0, &t0);
    }
    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);
    struct fe25519 zinv;
    fe_pow_pminus2(&zinv, &z2);
    fe_mul(&x2, &x2, &zinv);
    fe_to_bytes(out, x2);
    return TNU_TLS_OK;
}

void tnu_poly1305_mac(const uint8_t key[32], const uint8_t *msg,
                      size_t len, uint8_t tag[16])
{
    uint32_t t0, t1, t2, t3;
    uint64_t d0, d1, d2, d3, d4;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    uint32_t c;

    t0 = load_le32_tls(key + 0);
    t1 = load_le32_tls(key + 4);
    t2 = load_le32_tls(key + 8);
    t3 = load_le32_tls(key + 12);
    uint32_t r0 = t0 & 0x3ffffff;
    uint32_t r1 = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    uint32_t r2 = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    uint32_t r3 = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    uint32_t r4 = (t3 >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5;
    uint32_t s2 = r2 * 5;
    uint32_t s3 = r3 * 5;
    uint32_t s4 = r4 * 5;

    while (len) {
        uint8_t block[16];
        size_t n = len < 16 ? len : 16;
        memset(block, 0, sizeof(block));
        memcpy(block, msg, n);
        msg += n;
        len -= n;

        t0 = load_le32_tls(block + 0);
        t1 = load_le32_tls(block + 4);
        t2 = load_le32_tls(block + 8);
        t3 = load_le32_tls(block + 12);

        h0 += t0 & 0x3ffffff;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        h4 += (t3 >> 8) & 0x00ffffff;
        if (n == 16) {
            h4 += 1 << 24;
        } else {
            size_t bit = n * 8;
            if (bit < 26) {
                h0 += (uint32_t)1 << bit;
            } else if (bit < 52) {
                h1 += (uint32_t)1 << (bit - 26);
            } else if (bit < 78) {
                h2 += (uint32_t)1 << (bit - 52);
            } else if (bit < 104) {
                h3 += (uint32_t)1 << (bit - 78);
            } else {
                h4 += (uint32_t)1 << (bit - 104);
            }
        }

        d0 = ((uint64_t)h0 * r0) + ((uint64_t)h1 * s4) +
             ((uint64_t)h2 * s3) + ((uint64_t)h3 * s2) + ((uint64_t)h4 * s1);
        d1 = ((uint64_t)h0 * r1) + ((uint64_t)h1 * r0) +
             ((uint64_t)h2 * s4) + ((uint64_t)h3 * s3) + ((uint64_t)h4 * s2);
        d2 = ((uint64_t)h0 * r2) + ((uint64_t)h1 * r1) +
             ((uint64_t)h2 * r0) + ((uint64_t)h3 * s4) + ((uint64_t)h4 * s3);
        d3 = ((uint64_t)h0 * r3) + ((uint64_t)h1 * r2) +
             ((uint64_t)h2 * r1) + ((uint64_t)h3 * r0) + ((uint64_t)h4 * s4);
        d4 = ((uint64_t)h0 * r4) + ((uint64_t)h1 * r3) +
             ((uint64_t)h2 * r2) + ((uint64_t)h3 * r1) + ((uint64_t)h4 * r0);

        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    }

    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5;
    c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1 << 26);
    uint32_t mask = (g4 >> 31) - 1;
    uint32_t nmask = ~mask;
    h0 = (h0 & nmask) | (g0 & mask);
    h1 = (h1 & nmask) | (g1 & mask);
    h2 = (h2 & nmask) | (g2 & mask);
    h3 = (h3 & nmask) | (g3 & mask);
    h4 = (h4 & nmask) | (g4 & mask);

    uint64_t f0 = ((uint64_t)h0 | ((uint64_t)h1 << 26)) + load_le32_tls(key + 16);
    uint64_t f1 = ((uint64_t)(h1 >> 6) | ((uint64_t)h2 << 20)) + load_le32_tls(key + 20) + (f0 >> 32);
    uint64_t f2 = ((uint64_t)(h2 >> 12) | ((uint64_t)h3 << 14)) + load_le32_tls(key + 24) + (f1 >> 32);
    uint64_t f3 = ((uint64_t)(h3 >> 18) | ((uint64_t)h4 << 8)) + load_le32_tls(key + 28) + (f2 >> 32);
    store_le32_tls(tag + 0, (uint32_t)f0);
    store_le32_tls(tag + 4, (uint32_t)f1);
    store_le32_tls(tag + 8, (uint32_t)f2);
    store_le32_tls(tag + 12, (uint32_t)f3);
}

static void poly1305_update_padded(uint8_t *buf, size_t *pos,
                                   const uint8_t *data, size_t len)
{
    if (len) {
        memcpy(buf + *pos, data, len);
        *pos += len;
    }
    size_t pad = (16 - (len & 15)) & 15;
    memset(buf + *pos, 0, pad);
    *pos += pad;
}

int tnu_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *plain, size_t plain_len,
                               uint8_t *cipher, uint8_t tag[16])
{
    if ((!plain && plain_len) || (!cipher && plain_len) || (!aad && aad_len)) {
        return TNU_TLS_ERR_BAD_URL;
    }
    uint8_t otk[64];
    chacha20_block(key, nonce, 0, otk);
    tnu_chacha20_xor(key, nonce, 1, plain, cipher, plain_len);

    if (aad_len + plain_len + 32 > 2048) {
        return TNU_TLS_ERR_UNSUPPORTED;
    }
    uint8_t macbuf[2048];
    size_t pos = 0;
    poly1305_update_padded(macbuf, &pos, aad, aad_len);
    poly1305_update_padded(macbuf, &pos, cipher, plain_len);
    store_le64_tls(macbuf + pos, (uint64_t)aad_len);
    pos += 8;
    store_le64_tls(macbuf + pos, (uint64_t)plain_len);
    pos += 8;
    tnu_poly1305_mac(otk, macbuf, pos, tag);
    return TNU_TLS_OK;
}

int tnu_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *cipher, size_t cipher_len,
                               const uint8_t tag[16], uint8_t *plain)
{
    if ((!cipher && cipher_len) || (!plain && cipher_len) || (!aad && aad_len)) {
        return TNU_TLS_ERR_BAD_URL;
    }
    if (aad_len + cipher_len + 32 > 2048) {
        return TNU_TLS_ERR_UNSUPPORTED;
    }
    uint8_t otk[64];
    uint8_t calc[16];
    uint8_t macbuf[2048];
    size_t pos = 0;
    chacha20_block(key, nonce, 0, otk);
    poly1305_update_padded(macbuf, &pos, aad, aad_len);
    poly1305_update_padded(macbuf, &pos, cipher, cipher_len);
    store_le64_tls(macbuf + pos, (uint64_t)aad_len);
    pos += 8;
    store_le64_tls(macbuf + pos, (uint64_t)cipher_len);
    pos += 8;
    tnu_poly1305_mac(otk, macbuf, pos, calc);
    if (!bytes_eq(calc, tag, sizeof(calc))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    tnu_chacha20_xor(key, nonce, 1, cipher, plain, cipher_len);
    return TNU_TLS_OK;
}

const struct tnu_tls_features *tnu_tls_features(void)
{
    return &tls_features;
}

const char *tnu_tls_strerror(int error)
{
    switch (error) {
    case TNU_TLS_OK:
        return "ok";
    case TNU_TLS_ERR_CRYPTO_MISSING:
        return "TLS crypto backend missing";
    case TNU_TLS_ERR_NETWORK:
        return "TLS network error";
    case TNU_TLS_ERR_BAD_URL:
        return "bad HTTPS URL";
    case TNU_TLS_ERR_HANDSHAKE_INCOMPLETE:
        return "TLS handshake incomplete";
    case TNU_TLS_ERR_CERT_UNTRUSTED:
        return "certificate not trusted";
    case TNU_TLS_ERR_CERT_HOSTNAME:
        return "certificate hostname mismatch";
    case TNU_TLS_ERR_CERT_PARSE:
        return "certificate parse error";
    case TNU_TLS_ERR_UNSUPPORTED:
    default:
        return "TLS unsupported";
    }
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int tls_parse_https_url(const char *url, struct tls_url *out)
{
    if (!url || strncmp(url, "https://", 8) != 0 || !out) {
        return TNU_TLS_ERR_BAD_URL;
    }
    memset(out, 0, sizeof(*out));
    out->port = 443;

    const char *host = url + 8;
    const char *path = strchr(host, '/');
    const char *host_end = path ? path : host + strlen(host);
    const char *colon = NULL;
    for (const char *p = host; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    size_t host_len = (size_t)((colon ? colon : host_end) - host);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return TNU_TLS_ERR_BAD_URL;
    }
    memcpy(out->host, host, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        unsigned port = 0;
        const char *p = colon + 1;
        if (p == host_end) {
            return TNU_TLS_ERR_BAD_URL;
        }
        while (p < host_end) {
            if (*p < '0' || *p > '9') {
                return TNU_TLS_ERR_BAD_URL;
            }
            port = port * 10u + (unsigned)(*p - '0');
            if (port > 65535u) {
                return TNU_TLS_ERR_BAD_URL;
            }
            p++;
        }
        if (port == 0) {
            return TNU_TLS_ERR_BAD_URL;
        }
        out->port = (uint16_t)port;
    }

    if (path && path[0]) {
        strncpy(out->path, path, sizeof(out->path) - 1);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }
    return TNU_TLS_OK;
}

static int tls_tcp_connect(const struct tls_url *url)
{
    uint32_t ip = 0;
    if (resolve4(url->host, &ip) < 0) {
        return TNU_TLS_ERR_NETWORK;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return TNU_TLS_ERR_NETWORK;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(url->port);
    addr.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return TNU_TLS_ERR_NETWORK;
    }
    return fd;
}

static void tls_fill_random(uint8_t *out, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY, 0);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = read(fd, out + off, len - off);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        close(fd);
        if (off == len) {
            return;
        }
    }
    uint8_t seed[TNU_SHA256_DIGEST_SIZE];
    tnu_sha256("tiramisu-tls-beta-random", 24, seed);
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(seed[i % sizeof(seed)] + (uint8_t)i * 17u);
    }
}

static int tls_append_ext(uint8_t *buf, size_t *pos, size_t cap,
                          uint16_t type, const uint8_t *data, size_t len)
{
    if (*pos + 4 + len > cap || len > 65535u) {
        return TNU_TLS_ERR_UNSUPPORTED;
    }
    store_be16_tls(buf + *pos, type);
    *pos += 2;
    store_be16_tls(buf + *pos, (uint16_t)len);
    *pos += 2;
    if (len) {
        memcpy(buf + *pos, data, len);
        *pos += len;
    }
    return TNU_TLS_OK;
}

static int tls_send_client_hello(int fd, const struct tls_url *url)
{
    uint8_t body[512];
    size_t b = 0;
    body[b++] = 0x03;
    body[b++] = 0x03;
    tls_fill_random(body + b, 32);
    b += 32;

    body[b++] = 32;
    tls_fill_random(body + b, 32);
    b += 32;

    store_be16_tls(body + b, 4);
    b += 2;
    store_be16_tls(body + b, 0x1303);
    b += 2;
    store_be16_tls(body + b, 0x1301);
    b += 2;

    body[b++] = 1;
    body[b++] = 0;

    size_t ext_len_pos = b;
    b += 2;
    size_t ext_start = b;

    uint8_t sni[256];
    size_t host_len = strlen(url->host);
    if (host_len + 5 > sizeof(sni)) {
        return TNU_TLS_ERR_BAD_URL;
    }
    store_be16_tls(sni, (uint16_t)(host_len + 3));
    sni[2] = 0;
    store_be16_tls(sni + 3, (uint16_t)host_len);
    memcpy(sni + 5, url->host, host_len);
    int rc = tls_append_ext(body, &b, sizeof(body), 0x0000, sni, host_len + 5);
    if (rc < 0) return rc;

    uint8_t versions[] = { 2, 0x03, 0x04 };
    rc = tls_append_ext(body, &b, sizeof(body), 0x002b, versions, sizeof(versions));
    if (rc < 0) return rc;

    uint8_t groups[] = { 0, 2, 0, 0x1d };
    rc = tls_append_ext(body, &b, sizeof(body), 0x000a, groups, sizeof(groups));
    if (rc < 0) return rc;

    uint8_t sigalgs[] = {
        0, 8,
        0x08, 0x04,
        0x04, 0x03,
        0x08, 0x05,
        0x04, 0x01,
    };
    rc = tls_append_ext(body, &b, sizeof(body), 0x000d, sigalgs, sizeof(sigalgs));
    if (rc < 0) return rc;

    uint8_t key_share[38];
    store_be16_tls(key_share, 36);
    store_be16_tls(key_share + 2, 0x001d);
    store_be16_tls(key_share + 4, 32);
    tls_fill_random(key_share + 6, 32);
    rc = tls_append_ext(body, &b, sizeof(body), 0x0033, key_share, sizeof(key_share));
    if (rc < 0) return rc;

    uint8_t alpn[] = {
        0, 9,
        8, 'h', 't', 't', 'p', '/', '1', '.', '1',
    };
    rc = tls_append_ext(body, &b, sizeof(body), 0x0010, alpn, sizeof(alpn));
    if (rc < 0) return rc;

    store_be16_tls(body + ext_len_pos, (uint16_t)(b - ext_start));

    uint8_t record[5 + 4 + sizeof(body)];
    size_t p = 0;
    record[p++] = 22;
    record[p++] = 0x03;
    record[p++] = 0x01;
    store_be16_tls(record + p, (uint16_t)(4 + b));
    p += 2;
    record[p++] = 1;
    store_be24_tls(record + p, (uint32_t)b);
    p += 3;
    memcpy(record + p, body, b);
    p += b;

    return send(fd, record, p, 0) == (ssize_t)p ? TNU_TLS_OK : TNU_TLS_ERR_NETWORK;
}

static int tls_read_first_record(int fd, uint8_t *type_out)
{
    uint8_t hdr[5];
    ssize_t n = recv(fd, hdr, sizeof(hdr), 0);
    if (n != (ssize_t)sizeof(hdr)) {
        return TNU_TLS_ERR_NETWORK;
    }
    if (type_out) {
        *type_out = hdr[0];
    }
    uint16_t len = ((uint16_t)hdr[3] << 8) | hdr[4];
    uint8_t tmp[512];
    while (len) {
        size_t want = len < sizeof(tmp) ? len : sizeof(tmp);
        n = recv(fd, tmp, want, 0);
        if (n <= 0) {
            return TNU_TLS_ERR_NETWORK;
        }
        len -= (uint16_t)n;
    }
    return TNU_TLS_OK;
}

static int der_read_tlv(const uint8_t *der, size_t len, size_t *pos,
                        uint8_t *tag, const uint8_t **value, size_t *value_len)
{
    if (*pos + 2 > len) {
        return TNU_TLS_ERR_CERT_PARSE;
    }
    *tag = der[(*pos)++];
    uint8_t l = der[(*pos)++];
    size_t n = 0;
    if ((l & 0x80) == 0) {
        n = l;
    } else {
        size_t bytes = l & 0x7f;
        if (bytes == 0 || bytes > sizeof(size_t) || *pos + bytes > len) {
            return TNU_TLS_ERR_CERT_PARSE;
        }
        for (size_t i = 0; i < bytes; i++) {
            n = (n << 8) | der[(*pos)++];
        }
    }
    if (*pos + n > len) {
        return TNU_TLS_ERR_CERT_PARSE;
    }
    *value = der + *pos;
    *value_len = n;
    *pos += n;
    return TNU_TLS_OK;
}

static int tls_ascii_eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int tls_hostname_match(const char *pattern, const char *host)
{
    if (!pattern || !host || !pattern[0] || !host[0]) {
        return 0;
    }
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1;
        size_t host_len = strlen(host);
        size_t suffix_len = strlen(suffix);
        if (host_len <= suffix_len) {
            return 0;
        }
        const char *tail = host + host_len - suffix_len;
        if (!tls_ascii_eq_nocase(tail, suffix)) {
            return 0;
        }
        for (const char *p = host; p < tail; p++) {
            if (*p == '.') {
                return 0;
            }
        }
        return 1;
    }
    return tls_ascii_eq_nocase(pattern, host);
}

static void tls_copy_string(char *out, size_t out_size, const uint8_t *data, size_t len)
{
    size_t take = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, data, take);
    out[take] = '\0';
}

static void x509_scan_names(const uint8_t *der, size_t len, struct tnu_x509_info *out)
{
    static const uint8_t cn_oid[] = { 0x55, 0x04, 0x03 };
    for (size_t i = 0; i + sizeof(cn_oid) + 2 < len; i++) {
        if (der[i] == 0x06 && der[i + 1] == sizeof(cn_oid) &&
            memcmp(der + i + 2, cn_oid, sizeof(cn_oid)) == 0) {
            size_t p = i + 2 + sizeof(cn_oid);
            uint8_t tag;
            const uint8_t *value;
            size_t value_len;
            if (der_read_tlv(der, len, &p, &tag, &value, &value_len) == TNU_TLS_OK &&
                (tag == 0x0c || tag == 0x13 || tag == 0x16) &&
                out->common_name[0] == '\0') {
                tls_copy_string(out->common_name, sizeof(out->common_name),
                                value, value_len);
            }
        }
        if (der[i] == 0x82 && i + 1 < len && out->dns_name_count < 4) {
            size_t p = i;
            uint8_t tag;
            const uint8_t *value;
            size_t value_len;
            if (der_read_tlv(der, len, &p, &tag, &value, &value_len) == TNU_TLS_OK &&
                tag == 0x82 && value_len > 0 && value_len < TNU_X509_NAME_MAX) {
                tls_copy_string(out->dns_names[out->dns_name_count],
                                sizeof(out->dns_names[0]), value, value_len);
                out->dns_name_count++;
            }
        }
    }
}

int tnu_x509_parse_der(const uint8_t *der, size_t der_len, struct tnu_x509_info *out)
{
    if (!der || !out || der_len < 8) {
        return TNU_TLS_ERR_CERT_PARSE;
    }
    size_t pos = 0;
    uint8_t tag;
    const uint8_t *value;
    size_t value_len;
    if (der_read_tlv(der, der_len, &pos, &tag, &value, &value_len) < 0 ||
        tag != 0x30) {
        return TNU_TLS_ERR_CERT_PARSE;
    }
    memset(out, 0, sizeof(*out));
    tnu_sha256(der, der_len, out->fingerprint_sha256);
    x509_scan_names(der, der_len, out);
    return TNU_TLS_OK;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int parse_hex_sha256_line(const char *line, uint8_t out[32])
{
    int seen = 0;
    for (const char *p = line; *p && seen < 32;) {
        while (*p == ':' || *p == ' ' || *p == '\t') {
            p++;
        }
        int hi = hex_val((unsigned char)p[0]);
        int lo = hex_val((unsigned char)p[1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[seen++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    return seen == 32;
}

static int tls_trust_store_has_fingerprint(const uint8_t fp[32])
{
    int fd = open("/etc/ssl/certs/tnu-pins.txt", O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    char *line = buf;
    while (*line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        uint8_t parsed[32];
        if (parse_hex_sha256_line(line, parsed) && bytes_eq(parsed, fp, 32)) {
            return 1;
        }
        if (!next) {
            break;
        }
        line = next;
    }
    return 0;
}

int tnu_x509_validate_der(const uint8_t *der, size_t der_len, const char *hostname)
{
    struct tnu_x509_info info;
    int rc = tnu_x509_parse_der(der, der_len, &info);
    if (rc < 0) {
        return rc;
    }
    int host_ok = 0;
    if (info.dns_name_count) {
        for (size_t i = 0; i < info.dns_name_count; i++) {
            if (tls_hostname_match(info.dns_names[i], hostname)) {
                host_ok = 1;
                break;
            }
        }
    } else if (info.common_name[0]) {
        host_ok = tls_hostname_match(info.common_name, hostname);
    }
    if (!host_ok) {
        return TNU_TLS_ERR_CERT_HOSTNAME;
    }
    if (!tls_trust_store_has_fingerprint(info.fingerprint_sha256)) {
        return TNU_TLS_ERR_CERT_UNTRUSTED;
    }
    return TNU_TLS_OK;
}

int tnu_tls_selftest(void)
{
    static const uint8_t sha_abc[TNU_SHA256_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    static const uint8_t hmac_key[20] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b,
    };
    static const uint8_t hmac_expected[TNU_SHA256_DIGEST_SIZE] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    static const uint8_t chacha_expected[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
        0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
        0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
        0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
        0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    static const uint8_t poly_key[32] = {
        0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
        0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
        0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
        0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b,
    };
    static const uint8_t poly_expected[16] = {
        0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
        0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9,
    };
    static const uint8_t x25519_scalar[32] = {
        0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
        0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
        0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
        0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
    };
    static const uint8_t x25519_point[32] = {
        0x09, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    static const uint8_t x25519_expected[32] = {
        0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
        0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
        0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
        0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
    };
    uint8_t out[TNU_SHA256_DIGEST_SIZE];
    tnu_sha256("abc", 3, out);
    if (!bytes_eq(out, sha_abc, sizeof(out))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    tnu_hmac_sha256(hmac_key, sizeof(hmac_key), "Hi There", 8, out);
    if (!bytes_eq(out, hmac_expected, sizeof(out))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    uint8_t key[32];
    uint8_t nonce[12] = { 0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
    uint8_t zero[64];
    uint8_t stream[64];
    for (size_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)i;
    }
    memset(zero, 0, sizeof(zero));
    tnu_chacha20_xor(key, nonce, 1, zero, stream, sizeof(stream));
    if (!bytes_eq(stream, chacha_expected, sizeof(stream))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    uint8_t tag[16];
    tnu_poly1305_mac(poly_key,
                     (const uint8_t *)"Cryptographic Forum Research Group",
                     34, tag);
    if (!bytes_eq(tag, poly_expected, sizeof(tag))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    uint8_t shared[32];
    tnu_x25519(shared, x25519_scalar, x25519_point);
    if (!bytes_eq(shared, x25519_expected, sizeof(shared))) {
        return TNU_TLS_ERR_CRYPTO_MISSING;
    }
    return TNU_TLS_OK;
}

int tnu_https_get(const char *url, tnu_tls_write_cb write_cb, void *ctx)
{
    (void)write_cb;
    (void)ctx;
    struct tls_url parsed;
    int rc = tls_parse_https_url(url, &parsed);
    if (rc < 0) {
        return rc;
    }

    int fd = tls_tcp_connect(&parsed);
    if (fd < 0) {
        return fd;
    }
    rc = tls_send_client_hello(fd, &parsed);
    if (rc < 0) {
        close(fd);
        return rc;
    }
    uint8_t record_type = 0;
    rc = tls_read_first_record(fd, &record_type);
    close(fd);
    if (rc < 0) {
        return rc;
    }
    return record_type == 22 ? TNU_TLS_ERR_HANDSHAKE_INCOMPLETE
                             : TNU_TLS_ERR_CRYPTO_MISSING;
}
