/**
 * manifest.c — Object Manifest Layer Implementation
 *
 * Serialises/deserialises the ordered list of chunk hashes that describes
 * a logical object.  Provides CRC-32 integrity validation.
 */

#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  define MKDIR(p) mkdir((p), 0755)
#endif

/* -------------------------------------------------------------------------
 * CRC-32 (ISO 3309 / IEEE 802.3 polynomial 0xEDB88320)
 * ---------------------------------------------------------------------- */

static uint32_t crc32_table_g[256];
static int      crc32_table_ready = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        crc32_table_g[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t crc32_compute(const void *data, size_t len) {
    if (!crc32_table_ready) crc32_init_table();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table_g[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

/* -------------------------------------------------------------------------
 * Filesystem helpers
 * ---------------------------------------------------------------------- */

/** Recursively create all directories in `path`. */
static int mkdirs_for_file(const char *file_path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", file_path);

    /* Walk backwards to find the last separator. */
    char *last_sep = NULL;
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (!last_sep) return 0;
    *last_sep = '\0';  /* Temporarily truncate to directory. */

    /* Now recursively mkdir each component. */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p; *p = '\0';
            MKDIR(tmp);
            *p = save;
        }
    }
    MKDIR(tmp);
    return 0;
}

/** Write data to a temp file, then rename atomically. */
static int atomic_write(const char *path, const void *data, size_t size) {
    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;

    int ok = (size == 0 || fwrite(data, 1, size, f) == size);
    fclose(f);

    if (!ok) { remove(tmp_path); return -1; }

#ifdef _WIN32
    remove(path);
#endif
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return -1; }
    return 0;
}

/* -------------------------------------------------------------------------
 * Manifest entry array capacity management
 * ---------------------------------------------------------------------- */

/** Minimum initial capacity for the entries array. */
#define ENTRY_INIT_CAP 16

/* We store a hidden "capacity" before the entries pointer by using a wrapper.
 * For simplicity we embed capacity tracking in a thin heap-allocated block. */

typedef struct {
    size_t        capacity; /**< Allocated slots. */
    size_t        count;    /**< Used slots (same as header.num_chunks). */
    ManifestEntry entries[]; /**< Flexible array of entries. */
} EntryBlock;

/* -------------------------------------------------------------------------
 * API Implementation
 * ---------------------------------------------------------------------- */

Manifest *manifest_create(const char *key, const char *content_type) {
    Manifest *m = (Manifest *)calloc(1, sizeof(Manifest));
    if (!m) return NULL;

    m->header.magic   = MANIFEST_MAGIC;
    m->header.version = MANIFEST_VERSION;
    m->header.created_at  = (int64_t)time(NULL);
    m->header.modified_at = m->header.created_at;

    if (key)
        snprintf(m->header.key, sizeof(m->header.key), "%s", key);

    const char *ct = content_type ? content_type : "application/octet-stream";
    snprintf(m->header.content_type, sizeof(m->header.content_type), "%s", ct);

    /* Allocate initial entries array. */
    size_t initial = ENTRY_INIT_CAP;
    m->entries = (ManifestEntry *)calloc(initial, sizeof(ManifestEntry));
    if (!m->entries) { free(m); return NULL; }

    /* Store capacity in the first hidden slot convention:
     * We store it in header.num_chunks (which starts at 0), and track
     * capacity separately via a realloc strategy.
     * For cleanliness, we store capacity as a size_t in extra padding.
     * (Easier: just use realloc with num_chunks as count.) */

    return m;
}

int manifest_add_chunk(Manifest *m,
                       const char *hex,
                       uint64_t offset,
                       uint32_t size) {
    if (!m || !hex) return -1;

    uint64_t count = m->header.num_chunks;

    /* Grow entries array if needed (double capacity). */
    if (count >= ENTRY_INIT_CAP || count % ENTRY_INIT_CAP == 0) {
        size_t old_cap = count < ENTRY_INIT_CAP ? ENTRY_INIT_CAP : count;
        size_t new_cap = old_cap + ENTRY_INIT_CAP;
        ManifestEntry *ne = (ManifestEntry *)realloc(
            m->entries, new_cap * sizeof(ManifestEntry));
        if (!ne) return -1;
        m->entries = ne;
    }

    ManifestEntry *e = &m->entries[count];
    memset(e, 0, sizeof(*e));
    memcpy(e->hex, hex, SHA256_HEX_SIZE);
    e->offset = offset;
    e->size   = size;

    m->header.num_chunks++;
    m->header.object_size += size;
    m->header.modified_at  = (int64_t)time(NULL);

    return 0;
}

int manifest_write(const Manifest *m, const char *path) {
    if (!m || !path) return -1;

    mkdirs_for_file(path);

    /* Compute CRC-32 over the entry array. */
    size_t entries_sz = (size_t)m->header.num_chunks * sizeof(ManifestEntry);
    uint32_t crc = crc32_compute(m->entries, entries_sz);

    /* Build a mutable copy of the header with the CRC embedded. */
    ManifestHeader hdr = m->header;
    hdr.crc32_entries = crc;

    /* Allocate a flat buffer: header + entries. */
    size_t total = sizeof(ManifestHeader) + entries_sz;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    memcpy(buf, &hdr, sizeof(ManifestHeader));
    if (entries_sz > 0)
        memcpy(buf + sizeof(ManifestHeader), m->entries, entries_sz);

    int rc = atomic_write(path, buf, total);
    free(buf);
    return rc;
}

Manifest *manifest_read(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    ManifestHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f); return NULL;
    }

    if (hdr.magic != MANIFEST_MAGIC) {
        fprintf(stderr, "[manifest] Invalid magic in %s (got 0x%08X)\n",
                path, hdr.magic);
        fclose(f); return NULL;
    }
    if (hdr.version != MANIFEST_VERSION) {
        fprintf(stderr, "[manifest] Unsupported version %u in %s\n",
                hdr.version, path);
        fclose(f); return NULL;
    }

    size_t entries_sz = (size_t)hdr.num_chunks * sizeof(ManifestEntry);
    ManifestEntry *entries = NULL;

    if (entries_sz > 0) {
        entries = (ManifestEntry *)malloc(entries_sz);
        if (!entries) { fclose(f); return NULL; }
        if (fread(entries, 1, entries_sz, f) != entries_sz) {
            free(entries); fclose(f); return NULL;
        }
    }
    fclose(f);

    /* Validate CRC. */
    uint32_t crc = crc32_compute(entries, entries_sz);
    if (crc != hdr.crc32_entries) {
        fprintf(stderr, "[manifest] CRC mismatch in %s — data may be corrupt\n", path);
        free(entries);
        return NULL;
    }

    Manifest *m = (Manifest *)calloc(1, sizeof(Manifest));
    if (!m) { free(entries); return NULL; }

    m->header  = hdr;
    m->entries = entries;
    return m;
}

void manifest_free(Manifest *m) {
    if (!m) return;
    free(m->entries);
    free(m);
}

void manifest_print(const Manifest *m) {
    if (!m) { printf("(null manifest)\n"); return; }

    printf("Manifest:\n");
    printf("  key          : %s\n",  m->header.key);
    printf("  content_type : %s\n",  m->header.content_type);
    printf("  object_size  : %llu bytes\n",
           (unsigned long long)m->header.object_size);
    printf("  num_chunks   : %llu\n",
           (unsigned long long)m->header.num_chunks);
    printf("  created_at   : %lld\n",  (long long)m->header.created_at);
    printf("  modified_at  : %lld\n",  (long long)m->header.modified_at);
    printf("  Chunks:\n");
    for (uint64_t i = 0; i < m->header.num_chunks; i++) {
        const ManifestEntry *e = &m->entries[i];
        printf("    [%4llu] %.16s... offset=%-10llu size=%u\n",
               (unsigned long long)i,
               e->hex,
               (unsigned long long)e->offset,
               e->size);
    }
}
