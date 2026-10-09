/**
 * hashtable.c — Generic Open-Addressing Hash Table Implementation
 *
 * FNV-1a 64-bit hash, linear probing, tombstone deletion, load-factor rehash.
 */

#include "hashtable.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* -------------------------------------------------------------------------
 * FNV-1a 64-bit hash
 * ---------------------------------------------------------------------- */

#define FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME        UINT64_C(1099511628211)

static uint64_t fnv1a_64(const char *key) {
    uint64_t h = FNV_OFFSET_BASIS;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= (uint64_t)*p;
        h *= FNV_PRIME;
    }
    return h;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Return slot index for a given hash and capacity (power-of-two fast mod). */
static inline size_t slot_index(uint64_t hash, size_t capacity) {
    return (size_t)(hash & (uint64_t)(capacity - 1));
}

/** Duplicate a key using either the arena or heap. */
static char *key_dup(HashTable *ht, const char *key) {
    if (ht->arena)
        return arena_strdup(ht->arena, key);
    return strdup(key);
}

/** Free a key if it was heap-allocated (not arena-managed). */
static void key_free(HashTable *ht, char *key) {
    if (!ht->arena && key)
        free(key);
}

/**
 * ht_find_slot — locate the slot for `key` during a lookup or insert probe.
 *
 * Returns:
 *   - A pointer to an OCCUPIED slot whose key matches (hit), or
 *   - A pointer to an EMPTY slot where a new entry could be placed (miss), or
 *   - A pointer to a TOMBSTONE slot that is the best insert position (recycle).
 *
 * `found_out` is set to true on a hit.
 */
static HtSlot *ht_find_slot(HtSlot *slots, size_t capacity,
                             const char *key, uint64_t hash,
                             bool *found_out) {
    size_t   idx      = slot_index(hash, capacity);
    HtSlot  *tombstone = NULL;

    for (size_t i = 0; i < capacity; i++) {
        HtSlot *s = &slots[idx];

        if (s->state == HT_SLOT_EMPTY) {
            *found_out = false;
            return tombstone ? tombstone : s;
        }
        if (s->state == HT_SLOT_TOMBSTONE) {
            if (!tombstone) tombstone = s;
        } else if (s->hash == hash && strcmp(s->key, key) == 0) {
            *found_out = true;
            return s;
        }

        idx = (idx + 1) & (capacity - 1);
    }

    /* Table is full of tombstones — should never happen if rehashing is correct. */
    *found_out = false;
    return tombstone;
}

/**
 * ht_rehash — grow (or shrink) the bucket array to `new_capacity`.
 *
 * new_capacity must be a power-of-two.
 */
static bool ht_rehash(HashTable *ht, size_t new_capacity) {
    HtSlot *new_slots = (HtSlot *)calloc(new_capacity, sizeof(HtSlot));
    if (!new_slots) return false;

    /* Re-insert all live entries into the new array. */
    for (size_t i = 0; i < ht->capacity; i++) {
        HtSlot *old = &ht->slots[i];
        if (old->state != HT_SLOT_OCCUPIED) continue;

        bool found;
        HtSlot *dst = ht_find_slot(new_slots, new_capacity,
                                   old->key, old->hash, &found);
        assert(!found);  /* No duplicates; shouldn't find a match. */
        dst->state    = HT_SLOT_OCCUPIED;
        dst->key      = old->key;   /* Key pointer ownership transfers. */
        dst->value    = old->value;
        dst->hash     = old->hash;
    }

    free(ht->slots);
    ht->slots      = new_slots;
    ht->capacity   = new_capacity;
    ht->tombstones = 0;
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

HashTable *ht_create(Arena *arena, HtFreeFn free_fn) {
    HashTable *ht = (HashTable *)malloc(sizeof(HashTable));
    if (!ht) return NULL;

    ht->slots = (HtSlot *)calloc(HT_INITIAL_CAPACITY, sizeof(HtSlot));
    if (!ht->slots) {
        free(ht);
        return NULL;
    }

    ht->capacity   = HT_INITIAL_CAPACITY;
    ht->count      = 0;
    ht->tombstones = 0;
    ht->arena      = arena;
    ht->free_fn    = free_fn;
    return ht;
}

bool ht_insert(HashTable *ht, const char *key, void *value) {
    if (!ht || !key) return false;

    /* Rehash when (count + tombstones) / capacity >= load factor threshold. */
    if ((ht->count + ht->tombstones) * HT_LOAD_FACTOR_DEN >=
         ht->capacity * HT_LOAD_FACTOR_NUM) {
        size_t new_cap = ht->capacity * 2;
        if (!ht_rehash(ht, new_cap)) return false;
    }

    uint64_t hash = fnv1a_64(key);
    bool     found;
    HtSlot  *slot = ht_find_slot(ht->slots, ht->capacity, key, hash, &found);

    if (!slot) return false;

    if (found) {
        /* Update existing entry. */
        if (ht->free_fn && slot->value)
            ht->free_fn(slot->value);
        slot->value = value;
    } else {
        /* New entry — copy the key. */
        char *key_copy = key_dup(ht, key);
        if (!key_copy) return false;

        if (slot->state == HT_SLOT_TOMBSTONE)
            ht->tombstones--;

        slot->state = HT_SLOT_OCCUPIED;
        slot->key   = key_copy;
        slot->value = value;
        slot->hash  = hash;
        ht->count++;
    }

    return true;
}

void *ht_lookup(const HashTable *ht, const char *key) {
    if (!ht || !key) return NULL;

    uint64_t hash = fnv1a_64(key);
    bool     found;
    HtSlot  *slot = ht_find_slot(ht->slots, ht->capacity, key, hash, &found);

    return (found && slot) ? slot->value : NULL;
}

bool ht_delete(HashTable *ht, const char *key) {
    if (!ht || !key) return false;

    uint64_t hash = fnv1a_64(key);
    bool     found;
    HtSlot  *slot = ht_find_slot(ht->slots, ht->capacity, key, hash, &found);

    if (!found || !slot) return false;

    if (ht->free_fn && slot->value)
        ht->free_fn(slot->value);

    key_free(ht, slot->key);
    slot->key   = NULL;
    slot->value = NULL;
    slot->state = HT_SLOT_TOMBSTONE;
    ht->count--;
    ht->tombstones++;
    return true;
}

bool ht_contains(const HashTable *ht, const char *key) {
    if (!ht || !key) return false;
    uint64_t hash = fnv1a_64(key);
    bool     found;
    ht_find_slot(ht->slots, ht->capacity, key, hash, &found);
    return found;
}

void ht_iterate(const HashTable *ht, HtIterFn fn, void *user_data) {
    if (!ht || !fn) return;
    for (size_t i = 0; i < ht->capacity; i++) {
        const HtSlot *s = &ht->slots[i];
        if (s->state == HT_SLOT_OCCUPIED)
            fn(s->key, s->value, user_data);
    }
}

size_t ht_count(const HashTable *ht) {
    return ht ? ht->count : 0;
}

void ht_destroy(HashTable *ht) {
    if (!ht) return;

    for (size_t i = 0; i < ht->capacity; i++) {
        HtSlot *s = &ht->slots[i];
        if (s->state != HT_SLOT_OCCUPIED) continue;
        if (ht->free_fn && s->value)
            ht->free_fn(s->value);
        key_free(ht, s->key);
    }

    free(ht->slots);
    free(ht);
}
