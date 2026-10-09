/**
 * hashtable.h — Generic Open-Addressing Hash Table
 *
 * A lean, cache-friendly hash table that maps C-string keys to void* values.
 *
 * Design decisions:
 *   - Open addressing with linear probing (excellent cache locality).
 *   - FNV-1a non-cryptographic hash (fast, good distribution).
 *   - Tombstone deletion to preserve probe chains.
 *   - Load-factor threshold triggers automatic rehashing.
 *   - Optional arena integration: if an arena is supplied, key strings are
 *     duplicated into the arena; otherwise heap-allocated.
 */

#ifndef HELIX_HASHTABLE_H
#define HELIX_HASHTABLE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

/** Initial number of buckets (must be a power of two). */
#define HT_INITIAL_CAPACITY 64

/** Rehash when occupied / capacity exceeds this ratio. */
#define HT_LOAD_FACTOR_NUM  3
#define HT_LOAD_FACTOR_DEN  4   /* i.e. 0.75 */

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

/** Internal slot state. */
typedef enum {
    HT_SLOT_EMPTY     = 0,
    HT_SLOT_OCCUPIED  = 1,
    HT_SLOT_TOMBSTONE = 2
} HtSlotState;

/** A single slot in the hash table's bucket array. */
typedef struct HtSlot {
    HtSlotState  state;
    char        *key;    /**< Heap or arena copy of the key string. */
    void        *value;  /**< Caller-owned payload pointer. */
    uint64_t     hash;   /**< Cached 64-bit FNV-1a hash of `key`. */
} HtSlot;

/** Callback invoked for each live entry during iteration. */
typedef void (*HtIterFn)(const char *key, void *value, void *user_data);

/** Callback invoked when a value is evicted (deleted or overwritten). */
typedef void (*HtFreeFn)(void *value);

/**
 * HashTable — the public handle.
 */
typedef struct HashTable {
    HtSlot  *slots;       /**< Flat bucket array. */
    size_t   capacity;    /**< Total number of slots (always power-of-two). */
    size_t   count;       /**< Number of live (OCCUPIED) entries. */
    size_t   tombstones;  /**< Number of TOMBSTONE slots. */
    Arena   *arena;       /**< Optional arena for key copies (may be NULL). */
    HtFreeFn free_fn;     /**< Optional destructor for values (may be NULL). */
} HashTable;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * ht_create — allocate and initialise a new hash table.
 *
 * @param arena    Optional arena for key string storage; pass NULL to use
 *                 the system heap (keys will be strdup'd).
 * @param free_fn  Optional destructor called on evicted values; pass NULL
 *                 if values are unmanaged or managed elsewhere.
 * @return         Heap-allocated HashTable, or NULL on OOM.
 */
HashTable *ht_create(Arena *arena, HtFreeFn free_fn);

/**
 * ht_insert — insert or update a key/value pair.
 *
 * If `key` already exists its old value is replaced (free_fn is called on
 * the old value if registered).
 *
 * @param ht     Target hash table.
 * @param key    Null-terminated key string (copied internally).
 * @param value  Payload pointer to store.
 * @return       true on success, false on OOM.
 */
bool ht_insert(HashTable *ht, const char *key, void *value);

/**
 * ht_lookup — look up a key.
 *
 * @param ht   Hash table to search.
 * @param key  Key to look up.
 * @return     Stored value, or NULL if the key does not exist.
 */
void *ht_lookup(const HashTable *ht, const char *key);

/**
 * ht_delete — remove an entry by key.
 *
 * If free_fn was registered it is called on the value before removal.
 *
 * @param ht   Hash table.
 * @param key  Key to remove.
 * @return     true if the key was found and removed, false if not found.
 */
bool ht_delete(HashTable *ht, const char *key);

/**
 * ht_contains — test whether a key exists (no value retrieval).
 *
 * @param ht   Hash table.
 * @param key  Key to test.
 * @return     true if key is present.
 */
bool ht_contains(const HashTable *ht, const char *key);

/**
 * ht_iterate — call `fn(key, value, user_data)` for every live entry.
 *
 * Iteration order is unspecified.  Do not modify the table during iteration.
 *
 * @param ht        Hash table.
 * @param fn        Visitor callback.
 * @param user_data Opaque pointer forwarded to every `fn` call.
 */
void ht_iterate(const HashTable *ht, HtIterFn fn, void *user_data);

/**
 * ht_count — return number of live entries.
 */
size_t ht_count(const HashTable *ht);

/**
 * ht_destroy — free all internal resources.
 *
 * If free_fn was registered it is called for every remaining live value.
 * After this call `ht` is an invalid pointer.
 *
 * @param ht  Hash table to destroy (may be NULL — no-op).
 */
void ht_destroy(HashTable *ht);

#ifdef __cplusplus
}
#endif

#endif /* HELIX_HASHTABLE_H */
