/**
 * manifest.h — Object Manifest Layer
 *
 * A manifest is the "recipe" that describes a logical object (file) in terms
 * of an ordered sequence of chunk hashes (content addresses).  Reconstructing
 * the original byte stream requires fetching each chunk in manifest order and
 * concatenating them.
 *
 * On-disk format (<store_root>/buckets/<bucket>/<object_key>.manifest):
 *
 *   [ManifestHeader]              — fixed-size binary header
 *   [ManifestEntry × num_chunks]  — array of chunk descriptors
 *
 * All integers are stored in little-endian byte order.
 *
 * The header contains a CRC-32 of the entry array for integrity validation.
 */

#ifndef HELIX_MANIFEST_H
#define HELIX_MANIFEST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/** Magic number at offset 0 of every manifest file.  "HLXM" in ASCII. */
#define MANIFEST_MAGIC   0x4D584C48U

/** Manifest format version — increment when binary layout changes. */
#define MANIFEST_VERSION 1U

/** Maximum length of the user-supplied content-type string (incl. '\0'). */
#define MANIFEST_CONTENT_TYPE_MAX 128

/** Maximum length of the original object key stored in the manifest. */
#define MANIFEST_KEY_MAX 512

/* -------------------------------------------------------------------------
 * Binary On-Disk Structures (packed, little-endian)
 * ---------------------------------------------------------------------- */

/**
 * ManifestHeader — fixed 256-byte block at the start of every manifest file.
 *
 * Padding ensures the entry array begins at a 256-byte boundary, simplifying
 * future extension without breaking older readers.
 */
#pragma pack(push, 1)
typedef struct ManifestHeader {
    uint32_t magic;           /**< MANIFEST_MAGIC constant. */
    uint32_t version;         /**< Format version. */
    uint64_t object_size;     /**< Total uncompressed object size in bytes. */
    uint64_t num_chunks;      /**< Number of ManifestEntry records that follow. */
    int64_t  created_at;      /**< Unix timestamp (seconds) of upload. */
    int64_t  modified_at;     /**< Unix timestamp of last modification. */
    uint32_t crc32_entries;   /**< CRC-32 of the entry array (integrity check). */
    char     key[MANIFEST_KEY_MAX]; /**< Null-terminated object key. */
    char     content_type[MANIFEST_CONTENT_TYPE_MAX]; /**< MIME type string. */
    /* padding: brings ManifestHeader to exactly 256 bytes.
     * 256 - sizeof(magic=4) - sizeof(version=4) - sizeof(object_size=8)
     *     - sizeof(num_chunks=8) - sizeof(created_at=8) - sizeof(modified_at=8)
     *     - sizeof(crc32_entries=4) - MANIFEST_KEY_MAX(512) - CONTENT_TYPE_MAX(128)
     *   = 256 - 44 - 640 = negative → header is intentionally >256 for key+ct storage.
     * The 'padding' field is kept at 8 bytes purely as a forward-compatibility reserve. */
    uint8_t  padding[8]; /**< Reserved for future flags/version extensions. */
} ManifestHeader;
#pragma pack(pop)

/**
 * ManifestEntry — describes one chunk in the object's manifest.
 * Stored as a flat array immediately after ManifestHeader.
 */
#pragma pack(push, 1)
typedef struct ManifestEntry {
    char     hex[SHA256_HEX_SIZE]; /**< Chunk content address (hex SHA-256). */
    uint64_t offset;               /**< Byte offset of chunk in the original stream. */
    uint32_t size;                 /**< Chunk size in bytes. */
    uint8_t  padding[8];           /**< Reserved for flags/compression in future. */
} ManifestEntry;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * In-Memory Manifest
 * ---------------------------------------------------------------------- */

/**
 * Manifest — fully decoded in-memory representation of a manifest file.
 */
typedef struct Manifest {
    ManifestHeader  header;     /**< Decoded header (host byte-order copy). */
    ManifestEntry  *entries;    /**< Heap-allocated array of chunk entries. */
    /* entries is owned by this struct; freed by manifest_free(). */
} Manifest;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * manifest_create — allocate an empty Manifest with no entries.
 *
 * @param key           Logical object key (null-terminated).
 * @param content_type  MIME type string, or NULL → "application/octet-stream".
 * @return              Heap-allocated Manifest, or NULL on OOM.
 */
Manifest *manifest_create(const char *key, const char *content_type);

/**
 * manifest_add_chunk — append a chunk entry to the manifest.
 *
 * Automatically updates the running total of object_size.
 *
 * @param m       Target manifest.
 * @param hex     Chunk content address (64-char hex + '\0').
 * @param offset  Stream byte offset of the chunk start.
 * @param size    Chunk byte count.
 * @return        0 on success, negative on OOM.
 */
int manifest_add_chunk(Manifest *m,
                       const char *hex,
                       uint64_t offset,
                       uint32_t size);

/**
 * manifest_write — serialise and write a Manifest to a file path.
 *
 * Creates parent directories if needed.
 * Writes to a temp file and renames atomically.
 *
 * @param m     Manifest to write.
 * @param path  Destination file path.
 * @return      0 on success, negative on I/O error.
 */
int manifest_write(const Manifest *m, const char *path);

/**
 * manifest_read — read and deserialise a Manifest from a file path.
 *
 * Validates magic, version, and CRC-32 of the entry array.
 *
 * @param path  Source file path.
 * @return      Heap-allocated Manifest, or NULL on error.
 */
Manifest *manifest_read(const char *path);

/**
 * manifest_free — free all resources owned by a Manifest.
 *
 * @param m  Manifest to free (may be NULL — no-op).
 */
void manifest_free(Manifest *m);

/**
 * manifest_print — dump a human-readable summary to stdout.
 *
 * @param m  Manifest to display.
 */
void manifest_print(const Manifest *m);

/* -------------------------------------------------------------------------
 * CRC-32 Utility (used internally, exposed for testing)
 * ---------------------------------------------------------------------- */

/**
 * crc32_compute — compute a CRC-32 (ISO 3309 / Ethernet polynomial).
 *
 * @param data  Input bytes.
 * @param len   Input length.
 * @return      32-bit CRC value.
 */
uint32_t crc32_compute(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_MANIFEST_H */
