/**
 * sha256.h — Portable SHA-256 Implementation
 *
 * A self-contained, dependency-free SHA-256 implementation conforming to
 * FIPS 180-4.  Used to generate content-addressable names for stored chunks.
 *
 * Usage pattern:
 *   SHA256_CTX ctx;
 *   sha256_init(&ctx);
 *   sha256_update(&ctx, data, len);
 *   uint8_t digest[SHA256_DIGEST_SIZE];
 *   sha256_final(&ctx, digest);
 *
 * Convenience one-shot:
 *   sha256_hash(data, len, digest);
 *
 * Hex encoding:
 *   char hex[SHA256_HEX_SIZE];
 *   sha256_hex(digest, hex);
 */

#ifndef HELIX_SHA256_H
#define HELIX_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/** Raw digest length in bytes. */
#define SHA256_DIGEST_SIZE 32

/** Length of null-terminated hex string (64 hex chars + '\0'). */
#define SHA256_HEX_SIZE    65

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

/** Streaming SHA-256 context.  Treat as opaque. */
typedef struct {
    uint32_t state[8];   /**< Intermediate hash state. */
    uint64_t count;      /**< Total bytes processed. */
    uint8_t  buf[64];    /**< Partial block buffer. */
    int      buf_len;    /**< Bytes currently in buf. */
} SHA256_CTX;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * sha256_init — reset a context to start a new digest computation.
 */
void sha256_init(SHA256_CTX *ctx);

/**
 * sha256_update — feed bytes into the ongoing computation.
 *
 * @param ctx   Active context.
 * @param data  Input bytes.
 * @param len   Number of bytes to consume.
 */
void sha256_update(SHA256_CTX *ctx, const void *data, size_t len);

/**
 * sha256_final — finalise and write the 32-byte digest.
 *
 * The context becomes invalid after this call (re-init to reuse).
 *
 * @param ctx     Active context.
 * @param digest  Output buffer of at least SHA256_DIGEST_SIZE bytes.
 */
void sha256_final(SHA256_CTX *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * sha256_hash — one-shot convenience wrapper.
 *
 * @param data    Input data.
 * @param len     Input length.
 * @param digest  Output buffer of at least SHA256_DIGEST_SIZE bytes.
 */
void sha256_hash(const void *data, size_t len,
                 uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * sha256_hex — encode a raw digest to a lowercase hex C-string.
 *
 * @param digest  Raw 32-byte digest.
 * @param hex     Output buffer of at least SHA256_HEX_SIZE bytes.
 */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                char hex[SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_SHA256_H */
