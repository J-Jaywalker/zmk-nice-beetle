/*
 * PNG and animated GIF writers without external libraries.
 *
 * PNG: 8-bit grey or 24-bit RGB, per-row adaptive filter (None/Sub/Up/Paeth,
 * minimum sum of absolute differences), zlib stream compressed with a small
 * LZ77 + fixed-Huffman deflate encoder, CRC-32 and Adler-32 computed here.
 *
 * GIF: GIF89a, 256-level grey global palette, NETSCAPE2.0 infinite loop,
 * variable-width LZW (min code size 8).  One frame is buffered so identical
 * consecutive frames are merged (their delays add up) and each frame only
 * encodes the rectangle that changed since the previous one (disposal 1).
 */
#include "host.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ helpers */

struct buf {
    uint8_t *p;
    size_t n, cap;
    bool err;
};

static void buf_reserve(struct buf *b, size_t extra)
{
    if (b->err || b->n + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) {
        b->err = true;
        return;
    }
    b->p = p;
    b->cap = cap;
}

static void buf_put(struct buf *b, const void *d, size_t n)
{
    buf_reserve(b, n);
    if (b->err)
        return;
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void buf_byte(struct buf *b, uint8_t v)
{
    buf_put(b, &v, 1);
}

static uint32_t crc_table[256];
static bool crc_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *d, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = crc_table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32(const uint8_t *d, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *d++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------ deflate */

struct bitw {
    struct buf *out;
    uint32_t acc;
    int nbits;
};

static void bits_put(struct bitw *w, uint32_t v, int n) /* LSB first */
{
    w->acc |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        buf_byte(w->out, w->acc & 0xFF);
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

static void bits_huff(struct bitw *w, uint32_t code, int n) /* Huffman codes go MSB first */
{
    uint32_t r = 0;
    for (int i = 0; i < n; i++)
        r |= ((code >> i) & 1) << (n - 1 - i);
    bits_put(w, r, n);
}

static void bits_flush(struct bitw *w)
{
    if (w->nbits > 0)
        buf_byte(w->out, w->acc & 0xFF);
    w->acc = 0;
    w->nbits = 0;
}

static void fixed_lit(struct bitw *w, int v)
{
    if (v < 144)
        bits_huff(w, 0x30 + v, 8);
    else if (v < 256)
        bits_huff(w, 0x190 + (v - 144), 9);
    else if (v < 280)
        bits_huff(w, v - 256, 7);
    else
        bits_huff(w, 0xC0 + (v - 280), 8);
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                       33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void emit_match(struct bitw *w, int len, int dist)
{
    int i = 28;
    while (len_base[i] > len)
        i--;
    fixed_lit(w, 257 + i);
    if (len_extra[i])
        bits_put(w, len - len_base[i], len_extra[i]);
    int d = 29;
    while (dist_base[d] > dist)
        d--;
    bits_huff(w, d, 5);
    if (dist_extra[d])
        bits_put(w, dist - dist_base[d], dist_extra[d]);
}

#define WSIZE 32768
#define HBITS 15
#define HSIZE (1 << HBITS)
#define MAX_CHAIN 48

static uint32_t hash3(const uint8_t *p)
{
    return ((uint32_t)p[0] * 506832829u ^ (uint32_t)p[1] * 2654435761u ^ (uint32_t)p[2] * 40503u) >>
               7 & (HSIZE - 1);
}

/* zlib stream (header, one fixed-Huffman block, Adler-32) */
static int zlib_compress(const uint8_t *d, size_t n, struct buf *out)
{
    int32_t *head = malloc(sizeof(int32_t) * HSIZE);
    int32_t *prev = malloc(sizeof(int32_t) * WSIZE);
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    for (int i = 0; i < HSIZE; i++)
        head[i] = -1;

    buf_byte(out, 0x78);
    buf_byte(out, 0x01);
    struct bitw w = {out, 0, 0};
    bits_put(&w, 1, 1); /* BFINAL */
    bits_put(&w, 1, 2); /* BTYPE = fixed Huffman */

    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = hash3(d + i);
            int32_t c = head[h];
            int chain = MAX_CHAIN;
            size_t maxlen = n - i < 258 ? n - i : 258;
            while (c >= 0 && i - (size_t)c <= WSIZE - 1 && chain--) {
                const uint8_t *a = d + c, *b = d + i;
                if (a[best_len] == b[best_len] || best_len == 0) {
                    size_t l = 0;
                    while (l < maxlen && a[l] == b[l])
                        l++;
                    if ((int)l > best_len) {
                        best_len = (int)l;
                        best_dist = (int)(i - (size_t)c);
                        if (l == maxlen)
                            break;
                    }
                }
                c = prev[c & (WSIZE - 1)];
            }
            if (best_len < 3)
                best_len = 0;
        }
        size_t adv = best_len ? (size_t)best_len : 1;
        if (best_len)
            emit_match(&w, best_len, best_dist);
        else
            fixed_lit(&w, d[i]);
        /* insert hashes for every position consumed */
        for (size_t k = 0; k < adv; k++, i++) {
            if (i + 3 <= n) {
                uint32_t h = hash3(d + i);
                prev[i & (WSIZE - 1)] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    fixed_lit(&w, 256);
    bits_flush(&w);
    uint32_t ad = adler32(d, n);
    uint8_t t[4] = {ad >> 24, ad >> 16, ad >> 8, ad};
    buf_put(out, t, 4);
    free(head);
    free(prev);
    return out->err ? -1 : 0;
}

/* ------------------------------------------------------------ PNG */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static int png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_update(0, hdr + 4, 4);
    if (n)
        crc = crc32_update(crc, data, n);
    uint8_t c[4];
    put_be32(c, crc);
    if (fwrite(hdr, 1, 8, f) != 8)
        return -1;
    if (n && fwrite(data, 1, n, f) != n)
        return -1;
    return fwrite(c, 1, 4, f) == 4 ? 0 : -1;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static int png_write(const char *path, const uint8_t *img, int w, int h, int bpp)
{
    if (!path || !img || w <= 0 || h <= 0)
        return -1;
    size_t stride = (size_t)w * bpp;
    size_t rawn = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(rawn);
    uint8_t *cand = malloc(stride * 4);
    if (!raw || !cand) {
        free(raw);
        free(cand);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = img + stride * y;
        const uint8_t *up = y ? img + stride * (y - 1) : NULL;
        long best_sum = -1;
        int best = 0;
        for (int f = 0; f < 4; f++) {
            uint8_t *o = cand + stride * f;
            long sum = 0;
            for (size_t x = 0; x < stride; x++) {
                int a = x >= (size_t)bpp ? row[x - bpp] : 0;
                int b = up ? up[x] : 0;
                int c = (up && x >= (size_t)bpp) ? up[x - bpp] : 0;
                int pr = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : paeth(a, b, c);
                uint8_t v = (uint8_t)(row[x] - pr);
                o[x] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (best_sum < 0 || sum < best_sum) {
                best_sum = sum;
                best = f;
            }
        }
        uint8_t *dst = raw + (stride + 1) * y;
        dst[0] = (uint8_t)(best == 3 ? 4 : best); /* filter types: 0 none,1 sub,2 up,4 paeth */
        memcpy(dst + 1, cand + stride * best, stride);
    }
    free(cand);

    struct buf z = {0};
    int rc = zlib_compress(raw, rawn, &z);
    free(raw);
    if (rc) {
        free(z.p);
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z.p);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;                   /* bit depth */
    ihdr[9] = bpp == 1 ? 0 : 2;    /* colour type: grey / truecolour */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;
    if (!rc)
        rc = png_chunk(f, "IHDR", ihdr, 13);
    for (size_t off = 0; !rc && off < z.n; off += 1u << 20) {
        size_t k = z.n - off < (1u << 20) ? z.n - off : (1u << 20);
        rc = png_chunk(f, "IDAT", z.p + off, k);
    }
    if (!rc)
        rc = png_chunk(f, "IEND", NULL, 0);
    free(z.p);
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

int png_write_grey(const char *path, const uint8_t *img, int w, int h)
{
    return png_write(path, img, w, h, 1);
}

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    return png_write(path, rgb, w, h, 3);
}

/* ------------------------------------------------------------ GIF */

struct gif_writer {
    FILE *f;
    int w, h;
    bool err;
    uint8_t *shown;   /* canvas after the last written frame */
    uint8_t *pending; /* buffered frame not yet written */
    bool have_pending, have_shown;
    long pending_cs;
    /* LZW */
    int32_t hkey[8192];
    uint16_t hval[8192];
    uint8_t blk[256];
    int blkn;
    uint32_t acc;
    int nbits;
};

static void gif_bytes(struct gif_writer *g, const void *d, size_t n)
{
    if (!g->err && fwrite(d, 1, n, g->f) != n)
        g->err = true;
}

static void gif_le16(uint8_t *p, int v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void lzw_byte(struct gif_writer *g, uint8_t v)
{
    g->blk[1 + g->blkn++] = v;
    if (g->blkn == 255) {
        g->blk[0] = 255;
        gif_bytes(g, g->blk, 256);
        g->blkn = 0;
    }
}

static void lzw_code(struct gif_writer *g, int code, int size)
{
    g->acc |= (uint32_t)code << g->nbits;
    g->nbits += size;
    while (g->nbits >= 8) {
        lzw_byte(g, g->acc & 0xFF);
        g->acc >>= 8;
        g->nbits -= 8;
    }
}

static void lzw_reset(struct gif_writer *g)
{
    memset(g->hkey, 0xFF, sizeof g->hkey);
}

static int lzw_find(struct gif_writer *g, int32_t key)
{
    uint32_t h = ((uint32_t)key * 2654435761u) >> 19; /* 13 bits */
    while (g->hkey[h] != -1) {
        if (g->hkey[h] == key)
            return g->hval[h];
        h = (h + 1) & 8191;
    }
    return -1;
}

static void lzw_add(struct gif_writer *g, int32_t key, int code)
{
    uint32_t h = ((uint32_t)key * 2654435761u) >> 19;
    while (g->hkey[h] != -1)
        h = (h + 1) & 8191;
    g->hkey[h] = key;
    g->hval[h] = (uint16_t)code;
}

/* LZW-encode the rectangle (x0,y0,rw,rh) of img (stride g->w) */
static void lzw_encode(struct gif_writer *g, const uint8_t *img, int x0, int y0, int rw, int rh)
{
    const int clear = 256, eoi = 257;
    int size = 9, next = 258;
    uint8_t mcs = 8;
    gif_bytes(g, &mcs, 1);
    g->blkn = 0;
    g->acc = 0;
    g->nbits = 0;
    lzw_reset(g);
    lzw_code(g, clear, size);
    int cur = -1;
    for (int y = 0; y < rh; y++) {
        const uint8_t *row = img + (size_t)(y0 + y) * g->w + x0;
        for (int x = 0; x < rw; x++) {
            int c = row[x];
            if (cur < 0) {
                cur = c;
                continue;
            }
            int32_t key = (cur << 8) | c;
            int found = lzw_find(g, key);
            if (found >= 0) {
                cur = found;
                continue;
            }
            lzw_code(g, cur, size);
            if (next < 4096) {
                if (next == (1 << size))
                    size++;
                lzw_add(g, key, next++);
            } else {
                lzw_code(g, clear, size);
                lzw_reset(g);
                size = 9;
                next = 258;
            }
            cur = c;
        }
    }
    if (cur >= 0)
        lzw_code(g, cur, size);
    /* the decoder adds an entry after this code too and may widen first */
    if (next < 4096 && next == (1 << size))
        size++;
    lzw_code(g, eoi, size);
    if (g->nbits > 0)
        lzw_byte(g, g->acc & 0xFF);
    if (g->blkn) {
        g->blk[0] = (uint8_t)g->blkn;
        gif_bytes(g, g->blk, (size_t)g->blkn + 1);
    }
    uint8_t term = 0;
    gif_bytes(g, &term, 1);
}

static void gif_flush_pending(struct gif_writer *g)
{
    if (!g->have_pending)
        return;
    int x0 = 0, y0 = 0, x1 = g->w - 1, y1 = g->h - 1;
    if (g->have_shown) {
        /* bounding box of the change (non-empty: equal frames were merged) */
        x0 = g->w;
        y0 = g->h;
        x1 = -1;
        y1 = -1;
        for (int y = 0; y < g->h; y++) {
            const uint8_t *a = g->pending + (size_t)y * g->w, *b = g->shown + (size_t)y * g->w;
            if (!memcmp(a, b, g->w))
                continue;
            if (y < y0)
                y0 = y;
            y1 = y;
            for (int x = 0; x < g->w; x++)
                if (a[x] != b[x]) {
                    if (x < x0)
                        x0 = x;
                    if (x > x1)
                        x1 = x;
                }
        }
        if (x1 < 0) { /* cannot happen, but keep the file valid */
            x0 = y0 = x1 = y1 = 0;
        }
    }
    long cs = g->pending_cs;
    if (cs < 2)
        cs = 2;
    while (!g->err) {
        int d = cs > 65535 ? 65535 : (int)cs;
        cs -= d;
        uint8_t gce[8] = {0x21, 0xF9, 0x04, 0x04 /* disposal 1: leave in place */, 0, 0, 0, 0};
        gif_le16(gce + 4, d);
        gif_bytes(g, gce, 8);
        uint8_t desc[10] = {0x2C};
        gif_le16(desc + 1, x0);
        gif_le16(desc + 3, y0);
        gif_le16(desc + 5, x1 - x0 + 1);
        gif_le16(desc + 7, y1 - y0 + 1);
        desc[9] = 0;
        gif_bytes(g, desc, 10);
        lzw_encode(g, g->pending, x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        if (cs <= 0)
            break;
        /* very long delay: continue with a 1x1 no-change frame */
        x1 = x0;
        y1 = y0;
    }
    memcpy(g->shown, g->pending, (size_t)g->w * g->h);
    g->have_shown = true;
    g->have_pending = false;
}

struct gif_writer *gif_open(const char *path, int w, int h)
{
    if (!path || w <= 0 || h <= 0 || w > 65535 || h > 65535)
        return NULL;
    struct gif_writer *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->w = w;
    g->h = h;
    g->shown = malloc((size_t)w * h);
    g->pending = malloc((size_t)w * h);
    g->f = fopen(path, "wb");
    if (!g->shown || !g->pending || !g->f) {
        if (g->f)
            fclose(g->f);
        free(g->shown);
        free(g->pending);
        free(g);
        return NULL;
    }
    uint8_t hdr[13] = {'G', 'I', 'F', '8', '9', 'a'};
    gif_le16(hdr + 6, w);
    gif_le16(hdr + 8, h);
    hdr[10] = 0xF7; /* global colour table, 8-bit resolution, 256 entries */
    hdr[11] = 0;
    hdr[12] = 0;
    gif_bytes(g, hdr, 13);
    uint8_t pal[768];
    for (int i = 0; i < 256; i++)
        pal[i * 3] = pal[i * 3 + 1] = pal[i * 3 + 2] = (uint8_t)i;
    gif_bytes(g, pal, 768);
    static const uint8_t loop[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P',
                                     'E',  '2',  '.',  '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    gif_bytes(g, loop, 19);
    return g;
}

void gif_frame(struct gif_writer *g, const uint8_t *grey, int delay_cs)
{
    if (!g || !grey)
        return;
    if (delay_cs < 0)
        delay_cs = 0;
    size_t n = (size_t)g->w * g->h;
    if (g->have_pending && !memcmp(g->pending, grey, n)) {
        g->pending_cs += delay_cs; /* identical frame: just show the previous one longer */
        return;
    }
    gif_flush_pending(g);
    memcpy(g->pending, grey, n);
    g->pending_cs = delay_cs;
    g->have_pending = true;
}

void gif_close(struct gif_writer *g)
{
    if (!g)
        return;
    gif_flush_pending(g);
    uint8_t trailer = 0x3B;
    gif_bytes(g, &trailer, 1);
    fclose(g->f);
    free(g->shown);
    free(g->pending);
    free(g);
}
