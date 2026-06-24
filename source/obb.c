/* obb.c -- FF3 3D encrypted asset archive reader (main.obb)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>
#include <switch.h>

#include "obb.h"
#include "util.h"

#define OBB_LCG_MUL   0x41c64e6du
#define OBB_LCG_ADD   12345u
#define OBB_MAGIC     0x31435241u /* "ARC1" read as little-endian uint32 */

typedef struct {
  const char *name; // points into g_names
  uint32_t off;
  uint32_t len;
} ObbEntry;

static FILE *g_file = NULL;
static Mutex g_lock; // the engine calls loadFile from several threads
static char *g_names = NULL;     // the TOC name region (kept for entry->name)
static ObbEntry *g_entries = NULL;
static int g_count = 0;
static uint32_t g_seed = 0;      // LCG base seed, recovered per-obb in obb_open

// LCG keystream XOR, in place. byte[i] ^= (seed >> 24) after advancing.
static void obb_decode(uint8_t *buf, size_t len, uint32_t seed) {
  for (size_t i = 0; i < len; i++) {
    seed = seed * OBB_LCG_MUL + OBB_LCG_ADD;
    buf[i] ^= (uint8_t)(seed >> 24);
  }
}

static inline uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// modular inverse of an odd value mod 2^32 (Newton's iteration; each step
// doubles the number of correct low bits: 3 -> 6 -> 12 -> 24 -> 48 >= 32)
static uint32_t mul_inv(uint32_t a) {
  uint32_t x = a;
  for (int i = 0; i < 4; i++)
    x *= 2u - a * x;
  return x;
}

// decrypt the 16-byte header with a candidate seed and check it looks like ARC1
static int header_ok(const uint8_t *enc, uint32_t seed, long file_size) {
  uint8_t h[16];
  memcpy(h, enc, 16);
  obb_decode(h, 16, seed);
  if (rd_le32(h) != OBB_MAGIC)
    return 0;
  const uint32_t toc_off = rd_le32(h + 8);
  const uint32_t toc_len = rd_le32(h + 12);
  return toc_len > 0 && (uint64_t)toc_off + toc_len <= (uint64_t)file_size;
}

// The LCG seed differs between obb releases, so recover it from the file rather
// than hard-coding one: try the seeds we've already seen, then solve for the
// LCG state from the known "ARC1" magic (4 plaintext bytes pin the keystream).
static int recover_seed(const uint8_t *enc, long file_size, uint32_t *out) {
  static const uint32_t known[] = { 0x050EE91Au, 0x050EE21Au };
  for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    if (header_ok(enc, known[i], file_size)) { *out = known[i]; return 1; }

  const uint8_t k0 = enc[0] ^ 'A', k1 = enc[1] ^ 'R',
                k2 = enc[2] ^ 'C', k3 = enc[3] ^ '1';
  const uint32_t minv = mul_inv(OBB_LCG_MUL);
  for (uint32_t low = 0; low <= 0xFFFFFFu; low++) {
    uint32_t s1 = ((uint32_t)k0 << 24) | low;
    uint32_t s2 = s1 * OBB_LCG_MUL + OBB_LCG_ADD; if ((uint8_t)(s2 >> 24) != k1) continue;
    uint32_t s3 = s2 * OBB_LCG_MUL + OBB_LCG_ADD; if ((uint8_t)(s3 >> 24) != k2) continue;
    uint32_t s4 = s3 * OBB_LCG_MUL + OBB_LCG_ADD; if ((uint8_t)(s4 >> 24) != k3) continue;
    uint32_t base = (s1 - OBB_LCG_ADD) * minv;
    if (header_ok(enc, base, file_size)) { *out = base; return 1; }
  }
  return 0;
}

// read a raw blob from the file (locked); caller frees.
static uint8_t *read_raw(uint32_t off, uint32_t len) {
  uint8_t *buf = malloc(len ? len : 1);
  if (!buf)
    return NULL;
  if (fseek(g_file, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, g_file) != len) {
    free(buf);
    return NULL;
  }
  return buf;
}

// gunzip a [BE32 size][gzip] plaintext blob into a fresh buffer.
static void *unwrap(const uint8_t *blob, uint32_t bloblen, size_t *out_size) {
  if (bloblen < 4)
    return NULL;
  const uint32_t size = rd_be32(blob);
  uint8_t *out = malloc(size ? size : 1);
  if (!out)
    return NULL;

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  zs.next_in = (Bytef *)(blob + 4);
  zs.avail_in = bloblen - 4;
  zs.next_out = out;
  zs.avail_out = size;
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) { // 16 = gzip wrapper
    free(out);
    return NULL;
  }
  const int rc = inflate(&zs, Z_FINISH);
  inflateEnd(&zs);
  if (rc != Z_STREAM_END) {
    debugPrintf("obb: inflate failed (%d, got %lu of %u)\n", rc, (unsigned long)zs.total_out, size);
    free(out);
    return NULL;
  }
  if (out_size)
    *out_size = size;
  return out;
}

// read blob at (off,len), decrypt with seed base+off, gunzip. caller frees.
static void *read_blob(uint32_t off, uint32_t len, size_t *out_size) {
  uint8_t *raw = read_raw(off, len);
  if (!raw)
    return NULL;
  obb_decode(raw, len, g_seed + off);
  void *out = unwrap(raw, len, out_size);
  free(raw);
  return out;
}

static int entry_cmp(const void *a, const void *b) {
  return strcmp(((const ObbEntry *)a)->name, ((const ObbEntry *)b)->name);
}

int obb_open(const char *path) {
  mutexInit(&g_lock);

  g_file = fopen(path, "rb");
  if (!g_file) {
    debugPrintf("obb: cannot open %s\n", path);
    return -1;
  }

  long file_size = 0;
  if (fseek(g_file, 0, SEEK_END) == 0)
    file_size = ftell(g_file);
  rewind(g_file);

  // header: 16 bytes; the LCG seed varies per obb, so recover it from the magic
  uint8_t hdr[16];
  if (fread(hdr, 1, 16, g_file) != 16) {
    debugPrintf("obb: short read on header\n");
    goto fail;
  }
  if (!recover_seed(hdr, file_size, &g_seed)) {
    debugPrintf("obb: not an ARC1 archive (bad magic/seed)\n");
    goto fail;
  }
  obb_decode(hdr, 16, g_seed);
  const uint32_t toc_off = rd_le32(hdr + 8);
  const uint32_t toc_len = rd_le32(hdr + 12);

  size_t toc_size = 0;
  uint8_t *toc = read_blob(toc_off, toc_len, &toc_size);
  if (!toc || toc_size < 4) {
    debugPrintf("obb: failed to read TOC\n");
    free(toc);
    goto fail;
  }

  const uint32_t count = rd_le32(toc);
  const size_t name_region = 4 + (size_t)count * 12;
  if (name_region > toc_size) {
    debugPrintf("obb: TOC truncated (%u entries vs %lu bytes)\n", count, (unsigned long)toc_size);
    free(toc);
    goto fail;
  }

  g_names = (char *)toc; // keep the whole TOC; names point into it
  g_entries = malloc(sizeof(ObbEntry) * (count ? count : 1));
  if (!g_entries) {
    free(toc);
    g_names = NULL;
    goto fail;
  }
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *e = toc + 4 + (size_t)i * 12;
    const uint32_t noff = rd_le32(e);
    g_entries[i].name = (noff < toc_size) ? (g_names + noff) : "";
    g_entries[i].off = rd_le32(e + 4);
    g_entries[i].len = rd_le32(e + 8);
  }
  g_count = (int)count;
  // sort so we can bsearch regardless of the on-disk ordering
  qsort(g_entries, g_count, sizeof(ObbEntry), entry_cmp);

  return 0;

fail:
  fclose(g_file);
  g_file = NULL;
  return -1;
}

void obb_close(void) {
  if (g_file)
    fclose(g_file);
  g_file = NULL;
  free(g_entries);
  g_entries = NULL;
  free(g_names);
  g_names = NULL;
  g_count = 0;
  g_seed = 0;
}

static const ObbEntry *find(const char *name) {
  if (!g_entries || !name)
    return NULL;
  ObbEntry key;
  key.name = name;
  return bsearch(&key, g_entries, g_count, sizeof(ObbEntry), entry_cmp);
}

int obb_exists(const char *name) {
  return find(name) != NULL;
}

void *obb_read(const char *name, size_t *out_size) {
  if (!g_file || !g_entries)
    return NULL;
  mutexLock(&g_lock);
  const ObbEntry *e = find(name);
  void *out = NULL;
  if (e)
    out = read_blob(e->off, e->len, out_size);
  mutexUnlock(&g_lock);
  // misses are expected: the loadFile resolver probes several candidate paths
  return out;
}
