/* gzio.c — gzip / BGZF+tabix input for cinderplot.
 *
 * Two entry points:
 *   gz_read_all(path)        — whole-file gzip inflate into a NUL-terminated
 *                              buffer. zlib follows concatenated gzip members,
 *                              so this also reads bgzip. Used for small gzipped
 *                              TSVs (cytoband, seqinfo).
 *   tabix_slurp_region(...)  — a BGZF block reader driven by a .tbi index:
 *                              returns the uncompressed records whose index bins
 *                              overlap [beg,end). The caller re-filters for exact
 *                              overlap, so returning a coarse superset is fine.
 *
 * Only the read path of the tabix/BGZF spec is implemented — enough to serve a
 * single region query. No CSI, no writing. */
#include "cinderplot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

/* ---------------- whole-file gzip inflate ---------------- */
static char *gz_slurp(const char *path, size_t *outlen, char *err) {
    gzFile g = gzopen(path, "rb");
    if (!g) { sprintf(err, "cannot open %s", path); return NULL; }
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    for (;;) {
        if (cap - n < 4096) { cap *= 2; b = realloc(b, cap); }
        int r = gzread(g, b + n, (unsigned)(cap - n - 1));
        if (r < 0) { snprintf(err, 256, "%s: gzip read error", path); free(b); gzclose(g); return NULL; }
        n += (size_t)r;
        if (r == 0) break;
    }
    b[n] = 0;
    gzclose(g);
    if (outlen) *outlen = n;
    return b;
}

char *gz_read_all(const char *path, char *err) { return gz_slurp(path, NULL, err); }

/* ---------------- little-endian readers over a bounded buffer ------------- */
typedef struct { const unsigned char *p; size_t len, off; int ok; } Rd;
static uint32_t rd_u32(Rd *r) {
    if (r->off + 4 > r->len) { r->ok = 0; return 0; }
    const unsigned char *q = r->p + r->off; r->off += 4;
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}
static int32_t rd_i32(Rd *r) { return (int32_t)rd_u32(r); }
static uint64_t rd_u64(Rd *r) { uint64_t lo = rd_u32(r), hi = rd_u32(r); return lo | (hi << 32); }
static void rd_skip(Rd *r, size_t n) { if (r->off + n > r->len) r->ok = 0; else r->off += n; }

/* ---------------- BGZF single-block inflate ---------------- */
/* inflate the block at compressed offset `co`; malloc *out (uncompressed),
 * set *usize (uncompressed length) and *blocklen (compressed block size). */
static int bgzf_block(const unsigned char *c, size_t clen, size_t co,
                      unsigned char **out, uint32_t *usize, size_t *blocklen) {
    if (co + 12 > clen) return -1;
    if (c[co] != 0x1f || c[co + 1] != 0x8b) return -1;      /* gzip magic */
    unsigned xlen = c[co + 10] | (unsigned)c[co + 11] << 8;
    size_t xo = co + 12;
    if (xo + xlen > clen) return -1;
    unsigned bsize = 0; int have = 0;                        /* find the BC subfield */
    for (size_t i = 0; i + 4 <= xlen; ) {
        unsigned char si1 = c[xo + i], si2 = c[xo + i + 1];
        unsigned slen = c[xo + i + 2] | (unsigned)c[xo + i + 3] << 8;
        if (si1 == 'B' && si2 == 'C' && slen == 2) {
            bsize = (c[xo + i + 4] | (unsigned)c[xo + i + 5] << 8) + 1;
            have = 1; break;
        }
        i += 4 + slen;
    }
    if (!have || co + bsize > clen) return -1;
    size_t dstart = xo + xlen;                               /* deflate payload */
    size_t dlen = bsize - (xlen + 12) - 8;                   /* -header -extra -trailer */
    uint32_t isize = c[co + bsize - 4] | (uint32_t)c[co + bsize - 3] << 8
                   | (uint32_t)c[co + bsize - 2] << 16 | (uint32_t)c[co + bsize - 1] << 24;
    unsigned char *ub = malloc(isize ? isize : 1);
    if (isize) {
        z_stream zs; memset(&zs, 0, sizeof zs);
        if (inflateInit2(&zs, -15) != Z_OK) { free(ub); return -1; }
        zs.next_in = (unsigned char *)(c + dstart); zs.avail_in = (uInt)dlen;
        zs.next_out = ub; zs.avail_out = isize;
        int ret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (ret != Z_STREAM_END) { free(ub); return -1; }
    }
    *out = ub; *usize = isize; *blocklen = bsize;
    return 0;
}

/* ---------------- tabix binning ---------------- */
/* bins overlapping [beg,end); returns count written to list (<= cap). */
static int reg2bins(long beg, long end, int *list, int cap) {
    int i = 0; long k;
    if (beg < 0) beg = 0;
    if (end > (1L << 29)) end = 1L << 29;
    if (beg >= end) return 0;
    end--;
#define PUT(x) do { if (i < cap) list[i++] = (int)(x); } while (0)
    PUT(0);
    for (k = 1    + (beg >> 26); k <= 1    + (end >> 26); k++) PUT(k);
    for (k = 9    + (beg >> 23); k <= 9    + (end >> 23); k++) PUT(k);
    for (k = 73   + (beg >> 20); k <= 73   + (end >> 20); k++) PUT(k);
    for (k = 585  + (beg >> 17); k <= 585  + (end >> 17); k++) PUT(k);
    for (k = 4681 + (beg >> 14); k <= 4681 + (end >> 14); k++) PUT(k);
#undef PUT
    return i;
}

typedef struct { uint64_t b, e; } Chunk;
static int chunk_cmp(const void *a, const void *b) {
    uint64_t x = ((const Chunk *)a)->b, y = ((const Chunk *)b)->b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct { char *b; size_t n, cap; } Buf;
static void buf_add(Buf *bf, const unsigned char *s, size_t n) {
    if (bf->n + n + 1 > bf->cap) {
        if (!bf->cap) bf->cap = 1 << 16;
        while (bf->n + n + 1 > bf->cap) bf->cap *= 2;
        bf->b = realloc(bf->b, bf->cap);
    }
    memcpy(bf->b + bf->n, s, n); bf->n += n;
}

char *tabix_slurp_region(const char *path, const char *chrom, long beg, long end, char *err) {
    /* 1. the .tbi index is itself bgzip-compressed */
    char tbi[4096], ierr[256];
    snprintf(tbi, sizeof tbi, "%s.tbi", path);
    size_t ilen = 0;
    char *idx = gz_slurp(tbi, &ilen, ierr);
    if (!idx) { snprintf(err, 256, "%s: no tabix index (%s)", path, ierr); return NULL; }

    Rd rd = { (const unsigned char *)idx, ilen, 0, 1 };
    if (ilen < 4 || memcmp(idx, "TBI\1", 4)) { snprintf(err, 256, "%s: bad tabix magic", tbi); free(idx); return NULL; }
    rd.off = 4;
    int32_t n_ref = rd_i32(&rd);
    rd_i32(&rd);                                   /* format */
    rd_i32(&rd); rd_i32(&rd); rd_i32(&rd);         /* col_seq, col_beg, col_end */
    rd_i32(&rd); rd_i32(&rd);                      /* meta, skip */
    int32_t l_nm = rd_i32(&rd);
    if (!rd.ok || rd.off + (size_t)l_nm > ilen) { snprintf(err, 256, "%s: truncated tabix header", tbi); free(idx); return NULL; }

    /* find the reference id whose name matches chrom */
    const char *names = idx + rd.off;
    int target = -1;
    for (int id = 0; ; id++) {
        size_t o = 0; const char *nm = names;              /* walk NUL-separated names */
        for (int j = 0; j < id; j++) { size_t L = strlen(nm); o += L + 1; if (o >= (size_t)l_nm) { nm = NULL; break; } nm = names + o; }
        if (!nm || o >= (size_t)l_nm) break;
        if (!strcmp(nm, chrom)) { target = id; break; }
    }
    rd.off += (size_t)l_nm;
    if (target < 0) { free(idx); char *e = malloc(1); e[0] = 0; return e; }   /* chrom not indexed */

    int qcap = (int)((end - beg) >> 13) + 64;
    int *qbins = malloc(qcap * sizeof(int));
    int nq = reg2bins(beg, end, qbins, qcap);

    /* 2. walk refs, collecting chunks from matching bins + the linear-index min offset */
    Chunk *chunks = NULL; int nchunk = 0, ccap = 0;
    uint64_t min_off = 0;
    for (int ref = 0; ref < n_ref && rd.ok; ref++) {
        int32_t n_bin = rd_i32(&rd);
        for (int b = 0; b < n_bin && rd.ok; b++) {
            uint32_t bin = rd_u32(&rd);
            int32_t n_chunk = rd_i32(&rd);
            if (ref != target) { rd_skip(&rd, (size_t)n_chunk * 16); continue; }
            int want = 0;
            for (int q = 0; q < nq; q++) if ((uint32_t)qbins[q] == bin) { want = 1; break; }
            if (!want) { rd_skip(&rd, (size_t)n_chunk * 16); continue; }
            for (int ch = 0; ch < n_chunk && rd.ok; ch++) {
                uint64_t cb = rd_u64(&rd), ce = rd_u64(&rd);
                if (nchunk == ccap) { ccap = ccap ? ccap * 2 : 16; chunks = realloc(chunks, ccap * sizeof(Chunk)); }
                chunks[nchunk].b = cb; chunks[nchunk].e = ce; nchunk++;
            }
        }
        int32_t n_intv = rd_i32(&rd);
        if (ref != target) { rd_skip(&rd, (size_t)n_intv * 8); continue; }
        long iv = beg >> 14;
        for (int32_t k = 0; k < n_intv && rd.ok; k++) { uint64_t io = rd_u64(&rd); if (k == iv) min_off = io; }
        break;                                              /* target ref fully parsed */
    }
    free(qbins);
    free(idx);

    /* 3. filter by linear index, sort, merge overlapping chunks */
    int m = 0;
    for (int i = 0; i < nchunk; i++) if (chunks[i].e > min_off) chunks[m++] = chunks[i];
    nchunk = m;
    qsort(chunks, nchunk, sizeof(Chunk), chunk_cmp);
    int w = 0;
    for (int i = 0; i < nchunk; i++) {
        if (w && chunks[i].b <= chunks[w - 1].e) { if (chunks[i].e > chunks[w - 1].e) chunks[w - 1].e = chunks[i].e; }
        else chunks[w++] = chunks[i];
    }
    nchunk = w;

    /* 4. read the compressed file and inflate the merged chunk ranges */
    FILE *cf = fopen(path, "rb");
    if (!cf) { sprintf(err, "cannot open %s", path); free(chunks); return NULL; }
    fseek(cf, 0, SEEK_END); long csz = ftell(cf); fseek(cf, 0, SEEK_SET);
    unsigned char *cbuf = malloc(csz > 0 ? csz : 1);
    if (csz > 0 && fread(cbuf, 1, (size_t)csz, cf) != (size_t)csz) { sprintf(err, "%s: read error", path); fclose(cf); free(cbuf); free(chunks); return NULL; }
    fclose(cf);

    Buf ob = {0};
    for (int i = 0; i < nchunk; i++) {
        size_t co = chunks[i].b >> 16; unsigned uo = chunks[i].b & 0xffff;
        size_t co_end = chunks[i].e >> 16; unsigned uo_end = chunks[i].e & 0xffff;
        size_t pos = co;
        while (pos < (size_t)csz) {
            unsigned char *ub; uint32_t us; size_t blen;
            if (bgzf_block(cbuf, (size_t)csz, pos, &ub, &us, &blen) != 0) break;
            unsigned start = (pos == co) ? uo : 0;
            unsigned stop = (pos == co_end) ? uo_end : us;
            if (stop > us) stop = us;
            if (start < stop) buf_add(&ob, ub + start, stop - start);
            free(ub);
            if (pos == co_end || blen == 0) break;
            pos += blen;
        }
    }
    free(cbuf);
    free(chunks);

    if (!ob.b) { ob.b = malloc(1); ob.n = 0; }
    ob.b[ob.n] = 0;
    return ob.b;
}
