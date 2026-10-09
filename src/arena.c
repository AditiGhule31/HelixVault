/**
 * arena.c — Memory Pool / Arena Allocator Implementation
 */

#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Round `n` up to the nearest multiple of ARENA_ALIGNMENT. */
static inline size_t align_up(size_t n) {
    return (n + ARENA_ALIGNMENT - 1) & ~((size_t)(ARENA_ALIGNMENT - 1));
}

/**
 * slab_data — pointer to the first usable byte of a slab's payload region.
 *
 * The slab struct itself lives at the very start of the allocation; payload
 * starts immediately after it, aligned.
 */
static inline uint8_t *slab_data(ArenaSlab *slab) {
    return (uint8_t *)slab + align_up(sizeof(ArenaSlab));
}

/**
 * slab_new — allocate a fresh slab from the system heap.
 *
 * @param capacity  Usable payload bytes requested.
 * @return          Initialised ArenaSlab*, or NULL on OOM.
 */
static ArenaSlab *slab_new(size_t capacity) {
    /* We need space for the header + aligned payload. */
    size_t header_sz = align_up(sizeof(ArenaSlab));
    size_t total     = header_sz + capacity;

    ArenaSlab *slab = (ArenaSlab *)malloc(total);
    if (!slab) return NULL;

    slab->next     = NULL;
    slab->capacity = capacity;
    slab->used     = 0;
    return slab;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Arena *arena_create(size_t slab_size) {
    if (slab_size == 0)
        slab_size = ARENA_DEFAULT_SLAB_SIZE;

    Arena *arena = (Arena *)malloc(sizeof(Arena));
    if (!arena) return NULL;

    arena->head              = slab_new(slab_size);
    arena->default_slab_size = slab_size;
    arena->total_allocated   = 0;
    arena->total_wasted      = 0;

    if (!arena->head) {
        free(arena);
        return NULL;
    }
    return arena;
}

void *arena_alloc(Arena *arena, size_t size) {
    if (!arena) return NULL;

    /* Always return a unique pointer even for zero-size requests. */
    if (size == 0) size = 1;

    size_t aligned = align_up(size);
    size_t wasted  = aligned - size;

    /* Try the current slab first (common fast path). */
    ArenaSlab *slab = arena->head;
    if (slab && slab->used + aligned <= slab->capacity) {
        void *ptr     = slab_data(slab) + slab->used;
        slab->used   += aligned;
        arena->total_allocated += size;
        arena->total_wasted    += wasted;
        memset(ptr, 0, aligned);
        return ptr;
    }

    /* Need a new slab — make it at least `aligned` bytes large. */
    size_t new_cap = arena->default_slab_size;
    if (aligned > new_cap) new_cap = aligned;

    ArenaSlab *new_slab = slab_new(new_cap);
    if (!new_slab) return NULL;

    /* Prepend new slab to the list so it is tried first next time. */
    new_slab->next = arena->head;
    arena->head    = new_slab;

    void *ptr       = slab_data(new_slab);
    new_slab->used += aligned;
    arena->total_allocated += size;
    arena->total_wasted    += wasted;
    memset(ptr, 0, aligned);
    return ptr;
}

char *arena_strdup(Arena *arena, const char *s) {
    if (!arena || !s) return NULL;
    size_t len = strlen(s) + 1;  /* include NUL terminator */
    char  *dst = (char *)arena_alloc(arena, len);
    if (!dst) return NULL;
    memcpy(dst, s, len);
    return dst;
}

void arena_reset(Arena *arena) {
    if (!arena) return;

    /* Reset used counter in every slab but keep the memory. */
    for (ArenaSlab *s = arena->head; s; s = s->next)
        s->used = 0;

    arena->total_allocated = 0;
    arena->total_wasted    = 0;
}

void arena_destroy(Arena *arena) {
    if (!arena) return;

    ArenaSlab *slab = arena->head;
    while (slab) {
        ArenaSlab *next = slab->next;
        free(slab);
        slab = next;
    }
    free(arena);
}

void arena_stats(const Arena *arena, const char *label) {
    if (!label) label = "arena";
    if (!arena) {
        fprintf(stderr, "[arena_stats] %s: no arena\n", label);
        return;
    }

    /* Count slabs. */
    size_t slab_count = 0;
    for (ArenaSlab *s = arena->head; s; s = s->next)
        slab_count++;

    fprintf(stderr,
            "[arena_stats] %-20s | slabs=%zu  allocated=%zu B  wasted=%zu B\n",
            label, slab_count,
            arena->total_allocated,
            arena->total_wasted);
}
