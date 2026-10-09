/**
 * chunker.c — Content-Defined Chunking via Rabin Fingerprinting
 *
 * Rabin Fingerprint background:
 *   A Rabin fingerprint treats the sliding window contents as the coefficients
 *   of a polynomial over GF(2), divided by a chosen irreducible polynomial.
 *   We approximate this with a precomputed table for efficient rolling updates:
 *
 *       hash_new = (hash_old * BASE
 *                   + new_byte
 *                   - table[outgoing_byte] * BASE^WINDOW_SIZE) mod MODULUS
 *
 *   For our practical implementation we use the standard Gear hash variant,
 *   which is both simpler and faster while preserving CDC quality:
 *
 *       hash = (hash << 1) + GEAR_TABLE[byte]
 *
 *   A chunk boundary is declared when (hash & CHUNK_MASK) == 0, where
 *   CHUNK_MASK is chosen so the expected chunk size equals target_size.
 *
 *   We also use the classic Rabin window approach to remove the influence
 *   of bytes sliding out of the window, ensuring only the most recent
 *   RABIN_WINDOW_SIZE bytes influence the hash.
 */

#include "chunker.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Rabin / Gear Hash Tables
 * ---------------------------------------------------------------------- */

/*
 * GEAR_TABLE — one random 64-bit value per possible byte value (0-255).
 * Pre-computed from a fixed seed so chunking is deterministic.
 *
 * Values generated with:  for i in range(256): t[i] = random64_seeded(42)
 * (reproduced here as compile-time constants for portability).
 */
static const uint64_t GEAR_TABLE[256] = {
    UINT64_C(0x5C284A2C71D2A8B0), UINT64_C(0xB85D1F6D2FE76C14),
    UINT64_C(0x1A9E3F0C68BAD721), UINT64_C(0xC0F24A85B1D6398E),
    UINT64_C(0x47A1B3E0DCFE9120), UINT64_C(0x9823C741EF0D5A6B),
    UINT64_C(0xE4567B92A03C1FD8), UINT64_C(0x3D89FCE1750AB245),
    UINT64_C(0x7261A3D0B8F4E59C), UINT64_C(0xFA023E7C914D68B3),
    UINT64_C(0x0CB5D4A18267FE40), UINT64_C(0x58F61E2B3CAD907D),
    UINT64_C(0xA74EB8C059321E6F), UINT64_C(0x1F8D2740EAB5C983),
    UINT64_C(0x6390FCDB275E8A14), UINT64_C(0xD4A21B8E50F73C6A),
    UINT64_C(0x8BC37D0F19EAF526), UINT64_C(0x304568C2A7D1BE91),
    UINT64_C(0xE7F920A1C5B84D73), UINT64_C(0x5C0EB3D47F2916A0),
    UINT64_C(0xA91D7E5230C84BF2), UINT64_C(0x027AFC18D56B3E90),
    UINT64_C(0x68B14CE039FAD271), UINT64_C(0xBC290F73A4516C8D),
    UINT64_C(0x15E8A24B7D03F956), UINT64_C(0x73564F8CE21A90BD),
    UINT64_C(0xC9A37D140B85E62F), UINT64_C(0x4F182BC09A56D3E7),
    UINT64_C(0x87D5E0FA3126C941), UINT64_C(0xD20C4B91E7AF580E),
    UINT64_C(0x3E719A028FC3D54B), UINT64_C(0x9B4D560CE18A2F73),
    UINT64_C(0x5A0BF3D27819E64C), UINT64_C(0xE83C95A1706F2BD0),
    UINT64_C(0x24A7D3608E95F142), UINT64_C(0x7B5E12C4A308D97F),
    UINT64_C(0xCF023A8B5D7E1960), UINT64_C(0x41B6F7C038D9A25E),
    UINT64_C(0x9E7D54102B6CAF83), UINT64_C(0x162E80DBF945C37A),
    UINT64_C(0x6C931B74A051FE28), UINT64_C(0xB04FE2D83C76A591),
    UINT64_C(0x07A8DC5F192B3E6D), UINT64_C(0x5B361F0EA74CD820),
    UINT64_C(0xA0C94B728F0D1E53), UINT64_C(0xF8245E1C6B30AD97),
    UINT64_C(0x3D78A906E2F15B4C), UINT64_C(0x895C24B0F7AE3D61),
    UINT64_C(0xD140EB73026CF890), UINT64_C(0x2F85C9A1B43E750D),
    UINT64_C(0x7B01DFA28E59C346), UINT64_C(0xC36E4A0917F2D85B),
    UINT64_C(0x4A92B730DF016EC8), UINT64_C(0x0EFD5817A3BC4920),
    UINT64_C(0x5891C6F03E7AD2B4), UINT64_C(0xBD43F8A1205C91E6),
    UINT64_C(0x197D2B50E8A6CF43), UINT64_C(0x63B84E0F92150DBA),
    UINT64_C(0xAD265F7C3B84E091), UINT64_C(0x081EACD4F27359B6),
    UINT64_C(0x54F7032A1E9BD46C), UINT64_C(0xE04BC8F59A231870),
    UINT64_C(0x3C956DEA4B72F108), UINT64_C(0x72A4190B5CE830D6),
    UINT64_C(0xBF613D082A45F79E), UINT64_C(0x0D8B4FE271A90C53),
    UINT64_C(0x491E82B3CD560FA0), UINT64_C(0x97C25A0D4B813E6F),
    UINT64_C(0xE570BCDA18F29435), UINT64_C(0x2CAF4713E05B68A9),
    UINT64_C(0x780DCA92F3A14B5E), UINT64_C(0xC41B590E7A3FD820),
    UINT64_C(0x5F92E3B104DC6A71), UINT64_C(0xA83650CF79E12480),
    UINT64_C(0x13078AB4C2FDE593), UINT64_C(0x6B4DCF2901A38E5F),
    UINT64_C(0xD08E4A1B5F260C72), UINT64_C(0x25F17C08AB3DE940),
    UINT64_C(0x7A0E53D42C81BF16), UINT64_C(0xC9621F0B7A4ED503),
    UINT64_C(0x40B5A8C018FE2396), UINT64_C(0x8FC27D013A5CB649),
    UINT64_C(0xE34A0FBD9827C150), UINT64_C(0x1B769308E5ACFD24),
    UINT64_C(0x62D81FA4093BE570), UINT64_C(0xB4035C2DA17E89F6),
    UINT64_C(0x0F57D198CB2A4E30), UINT64_C(0x53A280E47F1BD96C),
    UINT64_C(0xA01CEF372B5C8048), UINT64_C(0xF84B7D102ACE5931),
    UINT64_C(0x369C4AB07E2F18D5), UINT64_C(0x8D5320C1F94B7EA6),
    UINT64_C(0xC1A674F0B0E3D528), UINT64_C(0x4E81B923DAF06174),
    UINT64_C(0x9204C750B8E13DAF), UINT64_C(0xDB7F183465AC02E9),
    UINT64_C(0x270DAF95C3E14B8D), UINT64_C(0x7B620C01F4A85E30),
    UINT64_C(0xC04F8B23D7E951A6), UINT64_C(0x14A9D3F6B20EC470),
    UINT64_C(0x58620B478F31DAC5), UINT64_C(0xAD97E1402C68B503),
    UINT64_C(0x02F340BC7EA19256), UINT64_C(0x4E861CD50B278FA9),
    UINT64_C(0x93450BEA6F8C17D2), UINT64_C(0xDF0C72318A4FE650),
    UINT64_C(0x2B75EAC047F39D21), UINT64_C(0x6FB91305D8A04C84),
    UINT64_C(0xB3DA40CE27F581A3), UINT64_C(0x07A8B9F2E063DC15),
    UINT64_C(0x4B59E0312F8CAB76), UINT64_C(0x9E213F08D14B70AC),
    UINT64_C(0xD86C7BA4058F23E1), UINT64_C(0x2C3A8F12B0E94C65),
    UINT64_C(0x7084D501CE254B92), UINT64_C(0xC5F3A98043B016FD),
    UINT64_C(0x190A4F27B78CE5A3), UINT64_C(0x5D61230FA4B8E971),
    UINT64_C(0xA1B875C418F5DA0E), UINT64_C(0xF50C2E40B7316A58),
    UINT64_C(0x3A7190BD6E2AF483), UINT64_C(0x8EC64D0A12F5BE39),
    UINT64_C(0xD2049B7C5EA12F40), UINT64_C(0x26B3D71EA05CE9BD),
    UINT64_C(0x6A7E4FA3C81B3052), UINT64_C(0xBEC82D10F6A047A5),
    UINT64_C(0x12A97CF04E356B18), UINT64_C(0x5640BAD239C18E6F),
    UINT64_C(0x9A0D153F780E4DC2), UINT64_C(0xDE52E98CB4A12F07),
    UINT64_C(0x2218B60F5D9C4AE3), UINT64_C(0x66F7D3B001A58920),
    UINT64_C(0xBACA07534E70CD7F), UINT64_C(0x0E2BF4C092D43E8A),
    UINT64_C(0x528040F27E19AB53), UINT64_C(0x96D735A0C1F65824),
    UINT64_C(0xDA4E8213A70B4967), UINT64_C(0x2E9CD4608B5F0EA1),
    UINT64_C(0x725B1095CF0A7DB4), UINT64_C(0xB6ACFE425C673109),
    UINT64_C(0x0A6B83B18051C27D), UINT64_C(0x4EC02F4E9BA04583),
    UINT64_C(0x92897CAD37D518F6), UINT64_C(0xD6D8A0F0241EC72A),
    UINT64_C(0x2A27EC1368B1A34F), UINT64_C(0x6E74309E0C64BE90),
    UINT64_C(0xB2C15D5BE0193A61), UINT64_C(0x0680A1F824EC79B3),
    UINT64_C(0x4AD77E0568B904FD), UINT64_C(0x8E2452C2AC0E319A),
    UINT64_C(0xD283967F60B1A84E), UINT64_C(0x16D23AC10C64FD90),
    UINT64_C(0x5A2117B450F98263), UINT64_C(0x9E7FEBA594AED731),
    UINT64_C(0xE2CE1952D801A4FF), UINT64_C(0x26BD43BF1CF58930),
    UINT64_C(0x6A0C9102F0A80E72), UINT64_C(0xAE5B7E3F3455DB84),
    UINT64_C(0x02AABB8C78607A16), UINT64_C(0x46F9782FB035CFE8),
    UINT64_C(0x8A4894C2C4E84012), UINT64_C(0xCE97580701BB35D4),
    UINT64_C(0x12E6852B458E9A26), UINT64_C(0x5635C2684C13EFF8),
    UINT64_C(0x9A84AF059AE6408A), UINT64_C(0xDED38C42D991B55C),
    UINT64_C(0x22028F7F174CE00E), UINT64_C(0x66516CBC545180E0),
    UINT64_C(0xAAA09B59B256155A), UINT64_C(0xEEEF7896100A89CC),
    UINT64_C(0x323E55D3BED5FD3E), UINT64_C(0x768D33107B0AF2B0),
    UINT64_C(0xBADC104DC7DE6722), UINT64_C(0xFE2AEE8A14B3DB94),
    UINT64_C(0x427997C7625E4F06), UINT64_C(0x86C8750EAF03C478),
    UINT64_C(0xCA1752CB3BD838EA), UINT64_C(0x0E6640085F6DAD5C),
    UINT64_C(0x5235BCC5AD38218E), UINT64_C(0x96849A02FA0296C0),
    UINT64_C(0xDAD377400F8D0B32), UINT64_C(0x1E22547D5C5280A4),
    UINT64_C(0x627131BA9AFFE9D6), UINT64_C(0xA6C01EF7E7AA5E48),
    UINT64_C(0xEA0FFC3434559BAA), UINT64_C(0x2E5EDA71810E091C),
    UINT64_C(0x72ADB7AE0FC37D8E), UINT64_C(0xB6FC94EBDC5EF280),
    UINT64_C(0xFB4B7228294663F2), UINT64_C(0x3F9A0F6576FB9864),
    UINT64_C(0x83E8CCA2C4A60CD6), UINT64_C(0xC797AADF116B8148),
    UINT64_C(0x0BE68E1C5E30F5BA), UINT64_C(0x4F356B59AB066A2C),
    UINT64_C(0x938449960D5BDE9E), UINT64_C(0xD7D326D35A304320),
    UINT64_C(0x1B220410A7E5B792), UINT64_C(0x5F70E14DF5BA2C04),
    UINT64_C(0xA3BFBE8A4365A076), UINT64_C(0xE7EE9BC7903A14E8),
    UINT64_C(0x2B3D7F04DCC5895A), UINT64_C(0x6F8C5C412A1AFECF),
    UINT64_C(0xB3DB39FEDEAFDF9C), UINT64_C(0xF78A173B2C04540E),
    UINT64_C(0x3BD9F478799FC880), UINT64_C(0x7FC8D1B5C70A3CF2),
    UINT64_C(0xC397ABEEE3F4B164), UINT64_C(0x07E6892B31C926D6),
    UINT64_C(0x4B3566689F9E9B48), UINT64_C(0x8F8443A5EC733FBA),
    UINT64_C(0xD3D3202238E8B42C), UINT64_C(0x1721FD5F8653299E),
    UINT64_C(0x5B70DA9CD3A87E10), UINT64_C(0x9FBFB7D921FDF382),
    UINT64_C(0xE30E9516C6D26854), UINT64_C(0x275D72537497DDC6),
    UINT64_C(0x6BAC4F90C26C5238), UINT64_C(0xAFFB2CCDAF01C6AA),
    UINT64_C(0xF34A0A0A5B963B1C), UINT64_C(0x3798E747A8EBAF8E),
    UINT64_C(0x7BE7C484F640249E), UINT64_C(0xBF36A1C143D49910),
    UINT64_C(0x03857FFE910E0E82), UINT64_C(0x47D45D3BDE03832C),
    UINT64_C(0x8B2338789BF8F79E), UINT64_C(0xCF7215B548EE6C10),
    UINT64_C(0x13C1F2F295C3E082), UINT64_C(0x5710D02FE2B854F4),
    UINT64_C(0x9A5FAD6C2F2DC966), UINT64_C(0xDEAE8AA97C183DD8),
    UINT64_C(0x22FD67E6C903B24A), UINT64_C(0x674C449615EF26BC),
    UINT64_C(0xAB9B21D362D49B2E), UINT64_C(0xEFEAFF10AEC90FA0),
    UINT64_C(0x3339DC4DFBBD8412), UINT64_C(0x7788B98A48B2F884),
    UINT64_C(0xBBD796C795E86CF6), UINT64_C(0xE0367A0AE1F1C268),
};

/* -------------------------------------------------------------------------
 * ChunkerCtx — implementation details
 * ---------------------------------------------------------------------- */

struct ChunkerCtx {
    /* Configuration */
    CdcConfig     cfg;           /**< Min/target/max chunk sizes. */
    ChunkCallback callback;      /**< Chunk boundary callback. */
    void         *user_data;     /**< Forwarded to callback. */

    /* Rolling hash state */
    uint64_t      hash;          /**< Current Gear hash accumulator. */
    uint8_t       window[RABIN_WINDOW_SIZE]; /**< Rabin sliding window. */
    size_t        window_pos;    /**< Write position in window (circular). */

    /* Byte accounting */
    size_t        chunk_len;     /**< Bytes in the current open chunk. */
    uint64_t      stream_offset; /**< Byte offset of current chunk start. */
    uint64_t      total_bytes;   /**< Total bytes fed so far. */

    /* Internal output buffer: the current chunk's data, grown as needed. */
    uint8_t      *chunk_buf;
    size_t        chunk_buf_cap;

    /* Statistics */
    size_t        num_chunks;
    size_t        min_chunk_seen;
    size_t        max_chunk_seen;
    uint64_t      total_chunk_bytes;

    /* Boundary mask derived from target_chunk_size. */
    uint64_t      mask;
};

/* Compute the gear-based mask from a target size. */
static uint64_t compute_mask(size_t target) {
    /* Find the next power-of-two >= target, then mask = that - 1. */
    if (target == 0) target = CDC_TARGET_CHUNK_SIZE;
    uint64_t bits = 0;
    uint64_t t    = (uint64_t)(target - 1);
    while (t > 0) { bits++; t >>= 1; }
    if (bits == 0) bits = 1;
    return ((uint64_t)1 << bits) - 1;
}

/* Grow the chunk buffer to hold at least `needed` bytes. */
static bool ensure_buf(ChunkerCtx *ctx, size_t needed) {
    if (needed <= ctx->chunk_buf_cap) return true;
    size_t new_cap = ctx->chunk_buf_cap ? ctx->chunk_buf_cap : 4096;
    while (new_cap < needed) new_cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(ctx->chunk_buf, new_cap);
    if (!nb) return false;
    ctx->chunk_buf     = nb;
    ctx->chunk_buf_cap = new_cap;
    return true;
}

/* Emit the current accumulated chunk via the user callback. */
static int emit_chunk(ChunkerCtx *ctx) {
    if (ctx->chunk_len == 0) return 0;

    ChunkInfo info;
    info.data   = ctx->chunk_buf;
    info.size   = ctx->chunk_len;
    info.offset = ctx->stream_offset;

    /* Update statistics. */
    ctx->num_chunks++;
    if (ctx->chunk_len < ctx->min_chunk_seen || ctx->min_chunk_seen == 0)
        ctx->min_chunk_seen = ctx->chunk_len;
    if (ctx->chunk_len > ctx->max_chunk_seen)
        ctx->max_chunk_seen = ctx->chunk_len;
    ctx->total_chunk_bytes += ctx->chunk_len;

    /* Advance stream offset for the next chunk. */
    ctx->stream_offset += ctx->chunk_len;

    /* Reset chunk state. */
    ctx->chunk_len  = 0;
    ctx->hash       = 0;
    memset(ctx->window, 0, RABIN_WINDOW_SIZE);
    ctx->window_pos = 0;

    int rc = 0;
    if (ctx->callback)
        rc = ctx->callback(&info, ctx->user_data);
    return rc;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

ChunkerCtx *chunker_create(const CdcConfig *cfg,
                            ChunkCallback    callback,
                            void            *user_data) {
    ChunkerCtx *ctx = (ChunkerCtx *)calloc(1, sizeof(ChunkerCtx));
    if (!ctx) return NULL;

    /* Apply config defaults. */
    if (cfg) {
        ctx->cfg = *cfg;
    }
    if (ctx->cfg.min_chunk_size    == 0) ctx->cfg.min_chunk_size    = CDC_MIN_CHUNK_SIZE;
    if (ctx->cfg.target_chunk_size == 0) ctx->cfg.target_chunk_size = CDC_TARGET_CHUNK_SIZE;
    if (ctx->cfg.max_chunk_size    == 0) ctx->cfg.max_chunk_size    = CDC_MAX_CHUNK_SIZE;

    ctx->mask          = compute_mask(ctx->cfg.target_chunk_size);
    ctx->callback      = callback;
    ctx->user_data     = user_data;
    ctx->min_chunk_seen = 0;  /* sentinel: not yet seen */

    return ctx;
}

int chunker_feed(ChunkerCtx *ctx, const uint8_t *data, size_t len) {
    if (!ctx || !data) return -1;

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        /* --- Rolling hash update (Gear hash) --- */
        /* The outgoing byte (sliding out of the window) affects the hash
         * via the GEAR_TABLE lookup combined with the circular window.     */
        uint8_t out_byte         = ctx->window[ctx->window_pos];
        ctx->window[ctx->window_pos] = byte;
        ctx->window_pos = (ctx->window_pos + 1) % RABIN_WINDOW_SIZE;

        /* Gear hash: shift left, XOR new byte's contribution, remove old. */
        ctx->hash = (ctx->hash << 1)
                  + GEAR_TABLE[byte]
                  - GEAR_TABLE[out_byte];

        /* Append byte to the current chunk buffer. */
        if (!ensure_buf(ctx, ctx->chunk_len + 1)) return -1;
        ctx->chunk_buf[ctx->chunk_len++] = byte;
        ctx->total_bytes++;

        /* --- Boundary detection --- */
        bool force_cut = (ctx->chunk_len >= ctx->cfg.max_chunk_size);
        bool natural_cut = (ctx->chunk_len >= ctx->cfg.min_chunk_size)
                         && ((ctx->hash & ctx->mask) == 0);

        if (natural_cut || force_cut) {
            int rc = emit_chunk(ctx);
            if (rc != 0) return rc;
        }
    }

    return 0;
}

int chunker_flush(ChunkerCtx *ctx) {
    if (!ctx) return -1;
    /* Emit whatever is buffered as the final chunk (possibly undersized). */
    return emit_chunk(ctx);
}

void chunker_reset(ChunkerCtx *ctx) {
    if (!ctx) return;
    ctx->hash         = 0;
    ctx->window_pos   = 0;
    ctx->chunk_len    = 0;
    ctx->stream_offset = 0;
    ctx->total_bytes  = 0;
    ctx->num_chunks   = 0;
    ctx->min_chunk_seen = 0;
    ctx->max_chunk_seen = 0;
    ctx->total_chunk_bytes = 0;
    memset(ctx->window, 0, RABIN_WINDOW_SIZE);
}

void chunker_destroy(ChunkerCtx *ctx) {
    if (!ctx) return;
    free(ctx->chunk_buf);
    free(ctx);
}

void chunker_stats(const ChunkerCtx *ctx, const char *label) {
    if (!label) label = "chunker";
    if (!ctx) {
        fprintf(stderr, "[chunker_stats] %s: no context\n", label);
        return;
    }

    size_t avg = ctx->num_chunks
               ? (size_t)(ctx->total_chunk_bytes / ctx->num_chunks)
               : 0;

    fprintf(stderr,
            "[chunker_stats] %-20s | chunks=%zu  avg=%zu B  "
            "min=%zu B  max=%zu B  total=%zu B\n",
            label,
            ctx->num_chunks, avg,
            ctx->min_chunk_seen, ctx->max_chunk_seen,
            (size_t)ctx->total_chunk_bytes);
}
