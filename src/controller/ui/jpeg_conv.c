/*
 * jpeg_conv.c — Lossless progressive-to-baseline JPEG transcoder.
 *
 * Approach: parse the progressive JPEG's multiple scans to accumulate the
 * quantized DCT coefficients, then write them back as a single-scan baseline
 * JPEG.  No IDCT / FDCT / pixel conversion is needed — coefficients and
 * quantization tables are preserved exactly.  The result is readable by
 * TJPGD and the ESP32-P4 hardware JPEG decoder.
 *
 * Memory budget (720×720 worst case):
 *   - Input file read   : ~200 KB  (PSRAM)
 *   - DCT coefficients  : ~1.5 MB  (PSRAM)
 *   - Output buffer     : ~300 KB  (PSRAM, allocated after input freed)
 * Peak ≈ 1.7 MB — fine with 32 MB PSRAM.
 */

#include "jpeg_conv.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "jpeg_conv";

/* ----- helpers --------------------------------------------------------- */

static void *psram_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sz);
    return p;
}
static void *psram_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(n, sz);
    return p;
}

/* ----- constants ------------------------------------------------------- */

#define MAX_COMP   3
#define MAX_HTAB   4   /* 2 DC + 2 AC */
#define MAX_QTAB   4

/* JPEG markers */
#define M_SOI  0xD8
#define M_SOF0 0xC0
#define M_SOF2 0xC2
#define M_DHT  0xC4
#define M_DQT  0xDB
#define M_SOS  0xDA
#define M_DRI  0xDD
#define M_RST0 0xD0
#define M_EOI  0xD9
#define M_APP0 0xE0

/* natural[zz] = 2D row-major position for zig-zag index zz */
static const uint8_t natural[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* ----- Huffman decode table -------------------------------------------- */

typedef struct {
    uint8_t  bits[17];      /* bits[i] = number of codes with length i */
    uint8_t  vals[256];     /* symbol values */
    int      maxcode[18];
    int      valptr[17];
    int      mincode[17];
    int      numsymbols;
} htab_t;

static void htab_build(htab_t *h)
{
    int code = 0, p = 0;
    for (int si = 1; si <= 16; si++) {
        h->mincode[si] = code;
        h->valptr[si] = p;
        for (int i = 0; i < h->bits[si]; i++) { code++; p++; }
        h->maxcode[si] = code - 1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->numsymbols = p;
}

/* ----- bitstream reader ------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    uint32_t       buf;
    int            bits;
} bits_t;

static void bits_init(bits_t *b, const uint8_t *data, size_t len)
{
    b->data = data; b->len = len; b->pos = 0;
    b->buf = 0; b->bits = 0;
}

static int bits_nextbyte(bits_t *b)
{
    while (b->pos < b->len) {
        uint8_t c = b->data[b->pos++];
        if (c != 0xFF) return c;
        if (b->pos >= b->len) return -1;
        uint8_t c2 = b->data[b->pos];
        if (c2 == 0x00) { b->pos++; return 0xFF; }
        if (c2 >= M_RST0 && c2 <= (M_RST0 + 7)) { b->pos++; continue; }
        b->pos--;
        return -1;
    }
    return -1;
}

static int bits_get(bits_t *b, int n)
{
    while (b->bits < n) {
        int c = bits_nextbyte(b);
        if (c < 0) c = 0;
        b->buf = (b->buf << 8) | (uint8_t)c;
        b->bits += 8;
    }
    b->bits -= n;
    return (int)((b->buf >> b->bits) & ((1u << n) - 1));
}

static int bits_getbool(bits_t *b) { return bits_get(b, 1); }

static int huff_decode(bits_t *b, const htab_t *h)
{
    int code = 0;
    for (int si = 1; si <= 16; si++) {
        code = (code << 1) | bits_getbool(b);
        if (code <= h->maxcode[si])
            return h->vals[h->valptr[si] + code - h->mincode[si]];
    }
    return 0;
}

static int extend(int v, int nbits)
{
    if (nbits == 0) return 0;
    if (v < (1 << (nbits - 1))) v -= (1 << nbits) - 1;
    return v;
}

static void refine_nonzero_coeff(bits_t *bs, int16_t *coef, uint8_t Al)
{
    int p1 = 1 << Al;
    bool correction = bits_getbool(bs);
    if (correction && (*coef & p1) == 0) {
        *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : -p1));
    }
}

/* ----- progressive JPEG context ---------------------------------------- */

typedef struct {
    uint8_t  id;
    uint8_t  hfact, vfact;
    uint8_t  qtab_id;
} comp_info_t;

typedef struct {
    uint16_t    width, height;
    uint8_t     ncomp;
    comp_info_t comp[MAX_COMP];
    uint16_t    qtab[MAX_QTAB][64];   /* zig-zag order from DQT */
    bool        qtab_present[MAX_QTAB];
    htab_t      dc_huff[2];
    htab_t      ac_huff[2];
    uint16_t    restart_interval;
    uint8_t     max_hfact, max_vfact;

    /* Quantized DCT coefficients in natural/2D order per block */
    int16_t    *coefs[MAX_COMP];
    uint32_t    block_count[MAX_COMP];
    uint16_t    blocks_x[MAX_COMP], blocks_y[MAX_COMP];

    const uint8_t *data;
    size_t         data_len;
} jpeg_ctx_t;

/* ----- marker parsing -------------------------------------------------- */

static const uint8_t *find_marker(const uint8_t *p, const uint8_t *end,
                                   uint8_t *out_marker)
{
    while (p + 1 < end) {
        if (p[0] == 0xFF && p[1] != 0x00 && p[1] != 0xFF) {
            *out_marker = p[1];
            return p + 2;
        }
        p++;
    }
    return NULL;
}

static bool parse_sof(jpeg_ctx_t *ctx, const uint8_t *seg, size_t len)
{
    if (len < 8 || seg[0] != 8) return false;
    ctx->height = (seg[1] << 8) | seg[2];
    ctx->width  = (seg[3] << 8) | seg[4];
    ctx->ncomp  = seg[5];
    if (ctx->ncomp > MAX_COMP || len < (size_t)(6 + ctx->ncomp * 3)) return false;
    ctx->max_hfact = ctx->max_vfact = 1;
    for (int i = 0; i < ctx->ncomp; i++) {
        ctx->comp[i].id      = seg[6 + i * 3];
        ctx->comp[i].hfact   = seg[7 + i * 3] >> 4;
        ctx->comp[i].vfact   = seg[7 + i * 3] & 0x0F;
        ctx->comp[i].qtab_id = seg[8 + i * 3];
        if (ctx->comp[i].hfact > ctx->max_hfact) ctx->max_hfact = ctx->comp[i].hfact;
        if (ctx->comp[i].vfact > ctx->max_vfact) ctx->max_vfact = ctx->comp[i].vfact;
    }
    uint16_t mcu_w = ctx->max_hfact * 8;
    uint16_t mcu_h = ctx->max_vfact * 8;
    uint16_t mcus_x = (ctx->width + mcu_w - 1) / mcu_w;
    uint16_t mcus_y = (ctx->height + mcu_h - 1) / mcu_h;
    for (int i = 0; i < ctx->ncomp; i++) {
        ctx->blocks_x[i] = mcus_x * ctx->comp[i].hfact;
        ctx->blocks_y[i] = mcus_y * ctx->comp[i].vfact;
        ctx->block_count[i] = (uint32_t)ctx->blocks_x[i] * ctx->blocks_y[i];
        ctx->coefs[i] = psram_calloc(ctx->block_count[i] * 64, sizeof(int16_t));
        if (!ctx->coefs[i]) { ESP_LOGE(TAG, "OOM coefs comp %d", i); return false; }
    }
    return true;
}

static bool parse_dqt(jpeg_ctx_t *ctx, const uint8_t *seg, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint8_t info = seg[off++];
        uint8_t prec = info >> 4;
        uint8_t id   = info & 0x0F;
        if (id >= MAX_QTAB) return false;
        size_t need = prec ? 128 : 64;
        if (off + need > len) return false;
        for (int i = 0; i < 64; i++) {
            if (prec) {
                ctx->qtab[id][i] = (seg[off] << 8) | seg[off + 1];
                off += 2;
            } else {
                ctx->qtab[id][i] = seg[off++];
            }
        }
        ctx->qtab_present[id] = true;
    }
    return true;
}

static bool parse_dht(jpeg_ctx_t *ctx, const uint8_t *seg, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint8_t info = seg[off++];
        uint8_t cls  = info >> 4;
        uint8_t id   = info & 0x0F;
        if (id > 1) return false;
        htab_t *h = cls ? &ctx->ac_huff[id] : &ctx->dc_huff[id];
        if (off + 16 > len) return false;
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            h->bits[i] = seg[off++];
            total += h->bits[i];
        }
        if (off + total > len) return false;
        memcpy(h->vals, &seg[off], total);
        off += total;
        htab_build(h);
    }
    return true;
}

/* ----- progressive scan decode ----------------------------------------- */

/* Decode one block's DC/AC coefficients from the bitstream. */
static void decode_block(jpeg_ctx_t *ctx, bits_t *bs, int16_t *blk,
                          int *dc_pred, int *eob_run,
                          uint8_t dc_sel, uint8_t ac_sel,
                          uint8_t Ss, uint8_t Se, uint8_t Ah, uint8_t Al)
{
    if (Ss == 0) {
        if (Ah == 0) {
            int sym = huff_decode(bs, &ctx->dc_huff[dc_sel]);
            int diff = 0;
            if (sym > 0) diff = extend(bits_get(bs, sym), sym);
            *dc_pred += diff;
            blk[0] = (int16_t)(*dc_pred << Al);
        } else {
            blk[0] |= (int16_t)(bits_getbool(bs) << Al);
        }
    }

    if (Se > 0) {
        if (Ah == 0) {
            /* first AC scan */
            if (*eob_run > 0) { (*eob_run)--; return; }
            for (int k = Ss; k <= Se; k++) {
                int sym = huff_decode(bs, &ctx->ac_huff[ac_sel]);
                int r = sym >> 4, s = sym & 0x0F;
                if (s == 0) {
                    if (r == 15) { k += 15; }
                    else {
                        *eob_run = (1 << r);
                        if (r > 0) *eob_run += bits_get(bs, r);
                        (*eob_run)--;
                        break;
                    }
                } else {
                    k += r;
                    if (k > Se) break;
                    int val = extend(bits_get(bs, s), s);
                    blk[natural[k]] = (int16_t)(val << Al);
                }
            }
        } else {
            /* refining AC scan */
            int p1 = 1 << Al;
            int k = Ss;
            if (*eob_run == 0) {
                for (; k <= Se; k++) {
                    int sym = huff_decode(bs, &ctx->ac_huff[ac_sel]);
                    int r = sym >> 4, s = sym & 0x0F;
                    if (s == 0) {
                        if (r < 15) {
                            *eob_run = (1 << r);
                            if (r > 0) *eob_run += bits_get(bs, r);
                            break;
                        }
                        int zeros = 16;
                        while (zeros > 0 && k <= Se) {
                            int16_t *coef = &blk[natural[k]];
                            if (*coef != 0) {
                                refine_nonzero_coeff(bs, coef, Al);
                            } else {
                                zeros--;
                            }
                            k++;
                        }
                        k--;
                    } else {
                        int val = bits_getbool(bs) ? p1 : -p1;
                        int zeros = r;
                        for (; k <= Se; k++) {
                            int16_t *coef = &blk[natural[k]];
                            if (*coef != 0) {
                                refine_nonzero_coeff(bs, coef, Al);
                                continue;
                            }
                            if (zeros == 0) {
                                *coef = (int16_t)val;
                                break;
                            }
                            zeros--;
                        }
                    }
                }
            }
            if (*eob_run > 0) {
                for (; k <= Se; k++) {
                    int16_t *coef = &blk[natural[k]];
                    if (*coef != 0) {
                        refine_nonzero_coeff(bs, coef, Al);
                    }
                }
                (*eob_run)--;
            }
        }
    }
}

static bool decode_progressive_scan(jpeg_ctx_t *ctx, const uint8_t *scan_data,
                                     size_t scan_len,
                                     uint8_t ncomp_scan, const uint8_t *comp_sel,
                                     const uint8_t *dc_sel, const uint8_t *ac_sel,
                                     uint8_t Ss, uint8_t Se, uint8_t Ah, uint8_t Al)
{
    bits_t bs;
    bits_init(&bs, scan_data, scan_len);

    int dc_pred[MAX_COMP] = {0};
    int eob_run = 0;
    uint16_t restart_counter = 0;

    /*
     * JPEG spec ITU-T T.81 §A.2:
     * - Interleaved scan (ncomp_scan > 1): data in MCU order, each MCU
     *   contains hfact×vfact blocks per component.
     * - Non-interleaved scan (ncomp_scan == 1): data in raster order of
     *   individual blocks (data units) for that single component.
     */

    if (ncomp_scan > 1) {
        /* ---- Interleaved scan ---- */
        uint16_t mcus_x = (ctx->width + ctx->max_hfact * 8 - 1) / (ctx->max_hfact * 8);
        uint16_t mcus_y = (ctx->height + ctx->max_vfact * 8 - 1) / (ctx->max_vfact * 8);
        uint32_t total_mcus = (uint32_t)mcus_x * mcus_y;

        for (uint32_t mcu = 0; mcu < total_mcus; mcu++) {
            if (ctx->restart_interval && restart_counter == ctx->restart_interval) {
                bs.bits = 0; bs.buf = 0;
                memset(dc_pred, 0, sizeof(dc_pred));
                eob_run = 0;
                restart_counter = 0;
            }

            uint16_t mcu_x = mcu % mcus_x;
            uint16_t mcu_y = mcu / mcus_x;

            for (int ci = 0; ci < ncomp_scan; ci++) {
                int c = comp_sel[ci];
                uint8_t hf = ctx->comp[c].hfact;
                uint8_t vf = ctx->comp[c].vfact;

                for (int vy = 0; vy < vf; vy++) {
                    for (int hx = 0; hx < hf; hx++) {
                        uint16_t bx = mcu_x * hf + hx;
                        uint16_t by = mcu_y * vf + vy;
                        uint32_t block_idx = (uint32_t)by * ctx->blocks_x[c] + bx;
                        int16_t *blk = &ctx->coefs[c][block_idx * 64];
                        decode_block(ctx, &bs, blk, &dc_pred[ci], &eob_run,
                                     dc_sel[ci], ac_sel[ci], Ss, Se, Ah, Al);
                    }
                }
            }
            restart_counter++;
        }
    } else {
        /* ---- Non-interleaved scan (single component, raster block order) ---- */
        int c = comp_sel[0];
        uint16_t bx_count = ctx->blocks_x[c];
        uint16_t by_count = ctx->blocks_y[c];
        uint32_t total_blocks = (uint32_t)bx_count * by_count;

        for (uint32_t blk_idx = 0; blk_idx < total_blocks; blk_idx++) {
            if (ctx->restart_interval && restart_counter == ctx->restart_interval) {
                bs.bits = 0; bs.buf = 0;
                dc_pred[0] = 0;
                eob_run = 0;
                restart_counter = 0;
            }

            int16_t *blk = &ctx->coefs[c][blk_idx * 64];
            decode_block(ctx, &bs, blk, &dc_pred[0], &eob_run,
                         dc_sel[0], ac_sel[0], Ss, Se, Ah, Al);
            restart_counter++;
        }
    }

    return true;
}

/* ======================================================================= */
/*  Baseline JPEG encoder (from quantized coefficients — lossless)         */
/* ======================================================================= */

/* Standard DC/AC Huffman tables (JPEG spec K.3) */
static const uint8_t dc_lum_bits[] = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t dc_lum_vals[] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t dc_chr_bits[] = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t dc_chr_vals[] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t ac_lum_bits[] = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t ac_lum_vals[] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,
    0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,
    0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,
    0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,
    0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,
    0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,
    0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,
    0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,
    0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,
    0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,
};
static const uint8_t ac_chr_bits[] = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t ac_chr_vals[] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,
    0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,
    0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,
    0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,
    0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,
    0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
    0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,
    0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
    0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa,
};

/* Encoder Huffman table: code[sym], length[sym] */
typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} enc_htab_t;

static void enc_htab_build(enc_htab_t *eh, const uint8_t *bits, const uint8_t *vals)
{
    memset(eh, 0, sizeof(*eh));
    int code = 0, idx = 0;
    for (int sz = 1; sz <= 16; sz++) {
        for (int i = 0; i < bits[sz]; i++) {
            uint8_t sym = vals[idx++];
            eh->code[sym] = (uint16_t)code;
            eh->len[sym] = (uint8_t)sz;
            code++;
        }
        code <<= 1;
    }
}

/* Bit-output buffer */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    uint32_t accum;
    int      nbits;
} bitout_t;

static void bo_init(bitout_t *bo, uint8_t *buf, size_t cap)
{
    bo->buf = buf; bo->cap = cap; bo->pos = 0;
    bo->accum = 0; bo->nbits = 0;
}

static void bo_putbyte(bitout_t *bo, uint8_t b)
{
    if (bo->pos < bo->cap) bo->buf[bo->pos++] = b;
    if (b == 0xFF && bo->pos < bo->cap) bo->buf[bo->pos++] = 0x00;
}

static void bo_putbits(bitout_t *bo, int code, int len)
{
    bo->accum = (bo->accum << len) | (code & ((1 << len) - 1));
    bo->nbits += len;
    while (bo->nbits >= 8) {
        bo->nbits -= 8;
        bo_putbyte(bo, (uint8_t)(bo->accum >> bo->nbits));
    }
}

static void bo_flush(bitout_t *bo)
{
    if (bo->nbits > 0) {
        bo_putbyte(bo, (uint8_t)(bo->accum << (8 - bo->nbits)));
        bo->nbits = 0;
    }
}

static void bo_raw(bitout_t *bo, const void *data, size_t n)
{
    if (bo->pos + n <= bo->cap) memcpy(&bo->buf[bo->pos], data, n);
    bo->pos += n;
}

/* Encode one 8×8 block of quantized coefficients (zig-zag order) */
static int encode_val_nbits(int val)
{
    if (val < 0) val = -val;
    int n = 0;
    while (val) { n++; val >>= 1; }
    return n;
}

static void encode_block(bitout_t *bo, const int16_t *block,
                          int *dc_pred,
                          const enc_htab_t *dc_ht,
                          const enc_htab_t *ac_ht)
{
    /* DC */
    int diff = block[0] - *dc_pred;
    *dc_pred = block[0];
    int nbits = encode_val_nbits(diff);
    bo_putbits(bo, dc_ht->code[nbits], dc_ht->len[nbits]);
    if (nbits > 0) {
        int val = diff < 0 ? diff - 1 + (1 << nbits) : diff;
        bo_putbits(bo, val, nbits);
    }
    /* AC */
    int last_nonzero = 63;
    while (last_nonzero > 0 && block[last_nonzero] == 0) last_nonzero--;
    if (last_nonzero == 0) {
        bo_putbits(bo, ac_ht->code[0x00], ac_ht->len[0x00]);
        return;
    }
    int zeros = 0;
    for (int i = 1; i <= last_nonzero; i++) {
        if (block[i] == 0) { zeros++; continue; }
        while (zeros >= 16) {
            bo_putbits(bo, ac_ht->code[0xF0], ac_ht->len[0xF0]);
            zeros -= 16;
        }
        int ac = block[i];
        nbits = encode_val_nbits(ac);
        int sym = (zeros << 4) | nbits;
        bo_putbits(bo, ac_ht->code[sym], ac_ht->len[sym]);
        int val = ac < 0 ? ac - 1 + (1 << nbits) : ac;
        bo_putbits(bo, val, nbits);
        zeros = 0;
    }
    if (last_nonzero < 63)
        bo_putbits(bo, ac_ht->code[0x00], ac_ht->len[0x00]);
}

/* Write DHT segment helper */
static void write_dht(bitout_t *bo, uint8_t cls_id,
                       const uint8_t *bits, const uint8_t *vals)
{
    int total = 0;
    for (int i = 1; i <= 16; i++) total += bits[i];
    uint16_t len = 2 + 1 + 16 + total;
    uint8_t h[] = {0xFF, M_DHT, (uint8_t)(len >> 8), (uint8_t)len, cls_id};
    bo_raw(bo, h, 5);
    bo_raw(bo, &bits[1], 16);
    bo_raw(bo, vals, total);
}

/* Transcode accumulated coefficients to a single-scan baseline JPEG */
static size_t transcode_baseline(jpeg_ctx_t *ctx, uint8_t *out, size_t out_cap)
{
    bitout_t bo;
    bo_init(&bo, out, out_cap);

    enc_htab_t edc_lum, edc_chr, eac_lum, eac_chr;
    enc_htab_build(&edc_lum, dc_lum_bits, dc_lum_vals);
    enc_htab_build(&edc_chr, dc_chr_bits, dc_chr_vals);
    enc_htab_build(&eac_lum, ac_lum_bits, ac_lum_vals);
    enc_htab_build(&eac_chr, ac_chr_bits, ac_chr_vals);

    /* SOI */
    uint8_t soi[] = {0xFF, M_SOI};
    bo_raw(&bo, soi, 2);

    /* APP0 (JFIF) */
    uint8_t app0[] = {0xFF, M_APP0, 0x00, 0x10, 'J','F','I','F',0,
                      1, 1, 0, 0x00, 0x01, 0x00, 0x01, 0, 0};
    bo_raw(&bo, app0, sizeof(app0));

    /* DQT — copy original quantization tables (already in zig-zag order) */
    uint8_t qtab_written = 0;
    for (int c = 0; c < ctx->ncomp; c++) {
        uint8_t qid = ctx->comp[c].qtab_id;
        if (qid >= MAX_QTAB || !ctx->qtab_present[qid]) continue;
        if (qtab_written & (1u << qid)) continue;
        qtab_written |= (1u << qid);

        /* check if any value > 255 (need 16-bit precision) */
        bool need16 = false;
        for (int i = 0; i < 64; i++)
            if (ctx->qtab[qid][i] > 255) { need16 = true; break; }

        uint16_t dqt_len = 2 + 1 + (need16 ? 128 : 64);
        uint8_t dqt_hdr[] = {0xFF, M_DQT,
                             (uint8_t)(dqt_len >> 8), (uint8_t)dqt_len,
                             (uint8_t)((need16 ? 0x10 : 0x00) | qid)};
        bo_raw(&bo, dqt_hdr, 5);
        for (int i = 0; i < 64; i++) {
            if (need16) {
                uint8_t v16[2] = {(uint8_t)(ctx->qtab[qid][i] >> 8),
                                  (uint8_t)(ctx->qtab[qid][i])};
                bo_raw(&bo, v16, 2);
            } else {
                uint8_t v8 = (uint8_t)ctx->qtab[qid][i];
                bo_raw(&bo, &v8, 1);
            }
        }
    }

    /* SOF0 (baseline DCT) — same dimensions and sampling as the progressive */
    {
        uint16_t sof_len = 2 + 1 + 2 + 2 + 1 + ctx->ncomp * 3;
        uint8_t sof[32] = {0xFF, M_SOF0,
                           (uint8_t)(sof_len >> 8), (uint8_t)sof_len,
                           8, /* 8-bit precision */
                           (uint8_t)(ctx->height >> 8), (uint8_t)ctx->height,
                           (uint8_t)(ctx->width >> 8),  (uint8_t)ctx->width,
                           ctx->ncomp};
        for (int i = 0; i < ctx->ncomp; i++) {
            sof[10 + i * 3] = ctx->comp[i].id;
            sof[11 + i * 3] = (ctx->comp[i].hfact << 4) | ctx->comp[i].vfact;
            sof[12 + i * 3] = ctx->comp[i].qtab_id;
        }
        bo_raw(&bo, sof, 10 + ctx->ncomp * 3);
    }

    /* DHT — standard Huffman tables */
    write_dht(&bo, 0x00, dc_lum_bits, dc_lum_vals);
    write_dht(&bo, 0x10, ac_lum_bits, ac_lum_vals);
    if (ctx->ncomp > 1) {
        write_dht(&bo, 0x01, dc_chr_bits, dc_chr_vals);
        write_dht(&bo, 0x11, ac_chr_bits, ac_chr_vals);
    }

    /* SOS — single scan with all components */
    {
        uint16_t sos_len = 2 + 1 + ctx->ncomp * 2 + 3;
        uint8_t sos[32] = {0xFF, M_SOS,
                           (uint8_t)(sos_len >> 8), (uint8_t)sos_len,
                           ctx->ncomp};
        for (int i = 0; i < ctx->ncomp; i++) {
            sos[5 + i * 2] = ctx->comp[i].id;
            /* Y → DC/AC table 0, Cb/Cr → DC/AC table 1 */
            sos[6 + i * 2] = (i == 0) ? 0x00 : 0x11;
        }
        int off = 5 + ctx->ncomp * 2;
        sos[off]     = 0;   /* Ss */
        sos[off + 1] = 63;  /* Se */
        sos[off + 2] = 0;   /* Ah=0, Al=0 */
        bo_raw(&bo, sos, off + 3);
    }

    /* Encode all MCUs — coefficients go from natural/2D order to zig-zag */
    uint16_t mcus_x = (ctx->width + ctx->max_hfact * 8 - 1) / (ctx->max_hfact * 8);
    uint16_t mcus_y = (ctx->height + ctx->max_vfact * 8 - 1) / (ctx->max_vfact * 8);

    int dc_pred[MAX_COMP] = {0};

    for (uint16_t my = 0; my < mcus_y; my++) {
        for (uint16_t mx = 0; mx < mcus_x; mx++) {
            for (int ci = 0; ci < ctx->ncomp; ci++) {
                uint8_t hf = ctx->comp[ci].hfact;
                uint8_t vf = ctx->comp[ci].vfact;
                const enc_htab_t *dc_ht = (ci == 0) ? &edc_lum : &edc_chr;
                const enc_htab_t *ac_ht = (ci == 0) ? &eac_lum : &eac_chr;

                for (int vy = 0; vy < vf; vy++) {
                    for (int hx = 0; hx < hf; hx++) {
                        uint16_t bx = mx * hf + hx;
                        uint16_t by = my * vf + vy;
                        uint32_t bidx = (uint32_t)by * ctx->blocks_x[ci] + bx;
                        const int16_t *blk = &ctx->coefs[ci][bidx * 64];

                        /* Reorder from natural/2D to zig-zag for encoder */
                        int16_t zz[64];
                        for (int i = 0; i < 64; i++)
                            zz[i] = blk[natural[i]];

                        encode_block(&bo, zz, &dc_pred[ci], dc_ht, ac_ht);
                    }
                }
            }
        }
    }

    bo_flush(&bo);
    uint8_t eoi[] = {0xFF, M_EOI};
    bo_raw(&bo, eoi, 2);
    return bo.pos;
}

/* ======================================================================= */
/*  Top-level: parse progressive, transcode to baseline                    */
/* ======================================================================= */

esp_err_t jpeg_progressive_to_baseline(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) { ESP_LOGW(TAG, "cannot open %s", filepath); return ESP_ERR_NOT_FOUND; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 4 || fsize > 4 * 1024 * 1024) {
        fclose(f); return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = psram_malloc(fsize);
    if (!data) { fclose(f); return ESP_ERR_NO_MEM; }
    /* Read in small chunks with yields so the SDMMC host controller can
     * service SDIO Wi-Fi (Slot 1) between SD card reads (Slot 0).       */
    {
        size_t off = 0;
        while (off < (size_t)fsize) {
            size_t chunk = (size_t)fsize - off;
            if (chunk > 1024) chunk = 1024;
            size_t got = fread(data + off, 1, chunk, f);
            if (got != chunk) { free(data); fclose(f); return ESP_FAIL; }
            off += chunk;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    fclose(f);

    /* Quick check: is this progressive (SOF2) or already baseline (SOF0)? */
    bool found_sof2 = false, found_sof0 = false;
    for (long i = 0; i < fsize - 1; i++) {
        if (data[i] == 0xFF) {
            if (data[i + 1] == M_SOF2) { found_sof2 = true; break; }
            if (data[i + 1] == M_SOF0) { found_sof0 = true; break; }
        }
    }
    if (!found_sof2) {
        free(data);
        return found_sof0 ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "transcoding %s (%ld bytes) progressive->baseline", filepath, fsize);

    jpeg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.data = data;
    ctx.data_len = fsize;

    /* Parse markers and decode progressive scans */
    const uint8_t *p = data;
    const uint8_t *end = data + fsize;
    uint8_t marker;

    p = find_marker(p, end, &marker);
    if (!p || marker != M_SOI) { free(data); return ESP_ERR_INVALID_ARG; }

    bool ok = true;
    while (ok && p && p < end) {
        p = find_marker(p, end, &marker);
        if (!p) break;
        if (marker == M_EOI) break;

        if (marker == M_SOS) {
            if (p + 2 > end) { ok = false; break; }
            uint16_t seg_len = (p[0] << 8) | p[1];
            const uint8_t *seg = p + 2;
            p += seg_len;
            if (seg + 1 > end) { ok = false; break; }

            uint8_t ns = seg[0];
            if (ns > MAX_COMP) { ok = false; break; }
            uint8_t comp_sel[MAX_COMP], dc_sel[MAX_COMP], ac_sel[MAX_COMP];
            for (int i = 0; i < ns; i++) {
                uint8_t cs = seg[1 + i * 2];
                uint8_t td_ta = seg[2 + i * 2];
                comp_sel[i] = 0;
                for (int j = 0; j < ctx.ncomp; j++)
                    if (ctx.comp[j].id == cs) { comp_sel[i] = j; break; }
                dc_sel[i] = td_ta >> 4;
                ac_sel[i] = td_ta & 0x0F;
            }
            uint8_t Ss = seg[1 + ns * 2];
            uint8_t Se = seg[2 + ns * 2];
            uint8_t Ahl = seg[3 + ns * 2];
            uint8_t Ah = Ahl >> 4, Al = Ahl & 0x0F;

            const uint8_t *scan_start = p;
            const uint8_t *sp = p;
            while (sp + 1 < end) {
                if (sp[0] == 0xFF && sp[1] != 0x00 &&
                    !(sp[1] >= M_RST0 && sp[1] <= M_RST0 + 7) &&
                    sp[1] != 0xFF)
                    break;
                sp++;
            }

            if (!decode_progressive_scan(&ctx, scan_start, sp - scan_start,
                                          ns, comp_sel, dc_sel, ac_sel,
                                          Ss, Se, Ah, Al))
                ESP_LOGW(TAG, "scan decode failed");
            p = sp;
            continue;
        }

        if (p + 2 > end) break;
        uint16_t seg_len = (p[0] << 8) | p[1];
        const uint8_t *seg = p + 2;
        p += seg_len;
        if (seg_len < 2) continue;
        size_t payload = seg_len - 2;

        switch (marker) {
            case M_SOF2: ok = parse_sof(&ctx, seg, payload); break;
            case M_DQT:  ok = parse_dqt(&ctx, seg, payload); break;
            case M_DHT:  ok = parse_dht(&ctx, seg, payload); break;
            case M_DRI:
                if (payload >= 2) ctx.restart_interval = (seg[0] << 8) | seg[1];
                break;
            default: break;
        }
    }

    if (!ok || ctx.width == 0 || ctx.height == 0) {
        ESP_LOGE(TAG, "failed to parse progressive JPEG");
        for (int i = 0; i < MAX_COMP; i++) free(ctx.coefs[i]);
        free(data);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "parsed %ux%u (%u components), transcoding", ctx.width, ctx.height, ctx.ncomp);

    /* Free input data before allocating output buffer */
    free(data); data = NULL;

    /* Allocate output — typically similar size to input */
    size_t out_cap = (size_t)fsize * 2;
    if (out_cap < 65536) out_cap = 65536;
    uint8_t *out = psram_malloc(out_cap);
    if (!out) {
        for (int i = 0; i < MAX_COMP; i++) free(ctx.coefs[i]);
        return ESP_ERR_NO_MEM;
    }

    size_t out_len = transcode_baseline(&ctx, out, out_cap);

    /* Free coefficient memory */
    for (int i = 0; i < MAX_COMP; i++) { free(ctx.coefs[i]); ctx.coefs[i] = NULL; }

    if (out_len == 0 || out_len >= out_cap) {
        free(out);
        ESP_LOGE(TAG, "transcode failed (len=%u, cap=%u)", (unsigned)out_len, (unsigned)out_cap);
        return ESP_FAIL;
    }

    /* Write back to file in small chunks with yields so the SDMMC host
     * controller can service SDIO Wi-Fi between SD card writes.          */
    f = fopen(filepath, "wb");
    if (!f) { free(out); return ESP_FAIL; }
    {
        size_t off = 0;
        bool write_ok = true;
        while (off < out_len) {
            size_t chunk = out_len - off;
            if (chunk > 1024) chunk = 1024;
            if (fwrite(out + off, 1, chunk, f) != chunk) {
                write_ok = false;
                break;
            }
            off += chunk;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (write_ok && fflush(f) != 0) write_ok = false;
        fclose(f);
        free(out);
        if (!write_ok) {
            ESP_LOGE(TAG, "write failed");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "transcoded %s: %ld -> %u bytes (lossless)", filepath, fsize, (unsigned)out_len);
    return ESP_OK;
}
