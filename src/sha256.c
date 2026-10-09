/**
 * sha256.c — Portable SHA-256 Implementation (FIPS 180-4)
 *
 * Self-contained; no dependency on OpenSSL or any system crypto library.
 * All integers are handled as big-endian on the wire (per the spec) using
 * explicit shift-based helpers so it works on both LE and BE hosts.
 */

#include "sha256.h"

#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * SHA-256 round constants (first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes).
 * ---------------------------------------------------------------------- */
static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

/* -------------------------------------------------------------------------
 * Bit-manipulation helpers
 * ---------------------------------------------------------------------- */

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

/* Big-endian load/store — safe for any host endianness. */
static inline uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static inline void be64_store(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >>  8); p[7] = (uint8_t)(v);
}

/* -------------------------------------------------------------------------
 * Core compression function (processes one 512-bit / 64-byte block)
 * ---------------------------------------------------------------------- */

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    int      i;

    /* Message schedule: first 16 words from the block. */
    for (i = 0; i < 16; i++)
        W[i] = be32_load(block + i * 4);

    /* Extend to 64 words. */
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(W[i-15], 7) ^ rotr32(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = rotr32(W[i-2], 17) ^ rotr32(W[i-2], 19)  ^ (W[i-2]  >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }

    /* Initialise working variables from current hash state. */
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    /* 64 rounds. */
    for (i = 0; i < 64; i++) {
        uint32_t S1    = rotr32(e, 6)  ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0    = rotr32(a, 2)  ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    /* Add the compressed chunk to the current hash value. */
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void sha256_init(SHA256_CTX *ctx) {
    /* Initial hash values: first 32 bits of the fractional parts of the
     * square roots of the first 8 primes. */
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->count   = 0;
    ctx->buf_len = 0;
}

void sha256_update(SHA256_CTX *ctx, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;

    while (len > 0) {
        /* How many bytes still fit in the current partial block? */
        int space = 64 - ctx->buf_len;
        int copy  = (len < (size_t)space) ? (int)len : space;

        memcpy(ctx->buf + ctx->buf_len, src, copy);
        ctx->buf_len += copy;
        ctx->count   += copy;
        src          += copy;
        len          -= copy;

        if (ctx->buf_len == 64) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
}

void sha256_final(SHA256_CTX *ctx, uint8_t digest[SHA256_DIGEST_SIZE]) {
    /* Append the mandatory '1' bit (0x80 byte). */
    ctx->buf[ctx->buf_len++] = 0x80;

    /* If not enough room for the 8-byte length, flush and start a new block. */
    if (ctx->buf_len > 56) {
        memset(ctx->buf + ctx->buf_len, 0, 64 - ctx->buf_len);
        sha256_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Pad with zeros up to byte 56 (leaving 8 bytes for the length). */
    memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);

    /* Append message length in bits as a 64-bit big-endian integer. */
    be64_store(ctx->buf + 56, ctx->count * 8ULL);

    sha256_compress(ctx->state, ctx->buf);

    /* Produce the final digest in big-endian byte order. */
    for (int i = 0; i < 8; i++)
        be32_store(digest + i * 4, ctx->state[i]);
}

void sha256_hash(const void *data, size_t len,
                 uint8_t digest[SHA256_DIGEST_SIZE]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                char hex[SHA256_HEX_SIZE]) {
    static const char HEXCHARS[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hex[i * 2 + 0] = HEXCHARS[digest[i] >> 4];
        hex[i * 2 + 1] = HEXCHARS[digest[i] & 0x0F];
    }
    hex[SHA256_HEX_SIZE - 1] = '\0';
}
