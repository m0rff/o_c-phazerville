// oc_doom_builtin.c - an IWAD packed into the Teensy 4.1's free program flash.
//
// software/lib_doom/pack_wad.py compresses a WAD in 32 KB chunks (raw
// DEFLATE) and the multiboot build places it at OCD_BUILTIN_ADDR, above the
// four 1 MB boot slots and below LittleFS. Flash is memory mapped, so reading
// is just decompressing the chunks a read touches, one chunk cached.
//
// Blob layout (little endian):
//   char  magic[8]      "OCWAD1\0\0"
//   char  name[16]      original file name, lower case (e.g. "doom1.wad")
//   u32   size          uncompressed size
//   u32   chunk_size
//   u32   num_chunks
//   u32   offset[num_chunks + 1]   chunk data offsets from the blob start
//   ...   chunk data

#include <stdint.h>
#include <string.h>
#include "oc_doom.h"

#ifndef OCD_BUILTIN_ADDR
#if defined(__IMXRT1062__)
#define OCD_BUILTIN_ADDR 0x60400000u
#endif
#endif

#define MAX_CHUNK 32768

typedef struct {
  char magic[8];
  char name[16];
  uint32_t size;
  uint32_t chunk_size;
  uint32_t num_chunks;
  uint32_t offset[1];
} builtin_header_t;

// Note: on the Teensy these statics live in PSRAM and are only valid after
// ocd_start() has initialized Doom's sections, so nothing here may rely on
// state kept from before that: the blob is looked up again on every call,
// and a cached chunk number of 0 means "nothing cached".
static const builtin_header_t *override_blob;   // desktop tests only
static char builtin_path[32];
static uint8_t chunk_buf[MAX_CHUNK];
static int32_t cached_chunk_plus1;

// ---------------------------------------------------------------------------
// Raw DEFLATE decoder (RFC 1951)
// ---------------------------------------------------------------------------

typedef struct {
  const uint8_t *in, *in_end;
  uint32_t bitbuf;
  int bitcount;
  uint8_t *out;
  uint32_t outpos, outlen;
} inflate_state_t;

typedef struct {
  uint16_t counts[16];   // codes per length
  uint16_t symbols[288]; // symbols in canonical order
} huffman_t;

static int GetBits(inflate_state_t *s, int n) {
  while (s->bitcount < n) {
    if (s->in >= s->in_end) return -1;
    s->bitbuf |= (uint32_t)(*s->in++) << s->bitcount;
    s->bitcount += 8;
  }
  int v = s->bitbuf & ((1u << n) - 1);
  s->bitbuf >>= n;
  s->bitcount -= n;
  return v;
}

static int BuildHuffman(huffman_t *h, const uint8_t *lengths, int n) {
  uint16_t offs[16];
  memset(h->counts, 0, sizeof(h->counts));
  for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
  h->counts[0] = 0;
  offs[1] = 0;
  for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->counts[i];
  for (int i = 0; i < n; i++)
    if (lengths[i]) h->symbols[offs[lengths[i]]++] = i;
  return 0;
}

static int Decode(inflate_state_t *s, const huffman_t *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    int b = GetBits(s, 1);
    if (b < 0) return -1;
    code |= b;
    int count = h->counts[len];
    if (code - count < first) return h->symbols[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                       8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int InflateBlock(inflate_state_t *s, const huffman_t *lit, const huffman_t *dist) {
  for (;;) {
    int sym = Decode(s, lit);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (s->outpos >= s->outlen) return -1;
      s->out[s->outpos++] = (uint8_t)sym;
    } else if (sym == 256) {
      return 0;
    } else {
      sym -= 257;
      if (sym >= 29) return -1;
      int e = GetBits(s, kLenExtra[sym]);
      if (e < 0) return -1;
      int len = kLenBase[sym] + e;
      int dsym = Decode(s, dist);
      if (dsym < 0 || dsym >= 30) return -1;
      e = GetBits(s, kDistExtra[dsym]);
      if (e < 0) return -1;
      uint32_t d = kDistBase[dsym] + e;
      if (d > s->outpos || s->outpos + len > s->outlen) return -1;
      for (int i = 0; i < len; i++, s->outpos++) s->out[s->outpos] = s->out[s->outpos - d];
    }
  }
}

static huffman_t lit_h, dist_h;   // static: too big for a comfortable stack

static int Inflate(const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outlen) {
  inflate_state_t s = {in, in + inlen, 0, 0, out, 0, outlen};
  int last;
  do {
    last = GetBits(&s, 1);
    int type = GetBits(&s, 2);
    if (last < 0 || type < 0) return -1;
    if (type == 0) {             // stored
      s.bitbuf = 0;
      s.bitcount = 0;
      if (s.in + 4 > s.in_end) return -1;
      uint32_t len = s.in[0] | (s.in[1] << 8);
      s.in += 4;
      if (s.in + len > s.in_end || s.outpos + len > s.outlen) return -1;
      memcpy(s.out + s.outpos, s.in, len);
      s.in += len;
      s.outpos += len;
    } else if (type == 1) {      // fixed Huffman
      uint8_t lengths[288];
      int i = 0;
      for (; i < 144; i++) lengths[i] = 8;
      for (; i < 256; i++) lengths[i] = 9;
      for (; i < 280; i++) lengths[i] = 7;
      for (; i < 288; i++) lengths[i] = 8;
      BuildHuffman(&lit_h, lengths, 288);
      for (i = 0; i < 30; i++) lengths[i] = 5;
      BuildHuffman(&dist_h, lengths, 30);
      if (InflateBlock(&s, &lit_h, &dist_h)) return -1;
    } else if (type == 2) {      // dynamic Huffman
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t lengths[320];
      int hlit = GetBits(&s, 5) + 257, hdist = GetBits(&s, 5) + 1, hclen = GetBits(&s, 4) + 4;
      if (hlit > 286 || hdist > 30) return -1;
      memset(lengths, 0, 19);
      for (int i = 0; i < hclen; i++) {
        int v = GetBits(&s, 3);
        if (v < 0) return -1;
        lengths[order[i]] = v;
      }
      BuildHuffman(&lit_h, lengths, 19);
      int n = 0;
      while (n < hlit + hdist) {
        int sym = Decode(&s, &lit_h);
        if (sym < 0) return -1;
        if (sym < 16) {
          lengths[n++] = sym;
        } else {
          int rep, val = 0;
          if (sym == 16) {
            if (n == 0) return -1;
            val = lengths[n - 1];
            rep = 3 + GetBits(&s, 2);
          } else if (sym == 17) {
            rep = 3 + GetBits(&s, 3);
          } else {
            rep = 11 + GetBits(&s, 7);
          }
          if (n + rep > hlit + hdist) return -1;
          while (rep--) lengths[n++] = val;
        }
      }
      BuildHuffman(&lit_h, lengths, hlit);
      BuildHuffman(&dist_h, lengths + hlit, hdist);
      if (InflateBlock(&s, &lit_h, &dist_h)) return -1;
    } else {
      return -1;
    }
  } while (!last);
  return (int)s.outpos;
}

// ---------------------------------------------------------------------------
// Built-in WAD access
// ---------------------------------------------------------------------------

static const builtin_header_t *Valid(const builtin_header_t *h) {
  if (!h || memcmp(h->magic, "OCWAD1\0\0", 8) != 0) return NULL;
  if (h->chunk_size == 0 || h->chunk_size > MAX_CHUNK) return NULL;
  if (h->num_chunks != (h->size + h->chunk_size - 1) / h->chunk_size) return NULL;
  return h;
}

static const builtin_header_t *Blob(void) {
#ifdef OCD_BUILTIN_ADDR
  return Valid((const builtin_header_t *)OCD_BUILTIN_ADDR);
#else
  return override_blob;
#endif
}

static void MakePath(const builtin_header_t *h, char *out) {
  char name[17];
  memcpy(name, h->name, 16);
  name[16] = 0;
  strcpy(out, "/flash/");
  strcat(out, name);
}

void ocd_builtin_set(const void *blob) {
  override_blob = Valid(blob);
  cached_chunk_plus1 = 0;
}

const char *ocd_builtin_path(void) {
  const builtin_header_t *h = Blob();
  if (!h) return NULL;
  MakePath(h, builtin_path);
  return builtin_path;
}

int ocd_builtin_is(const char *path) {
  const builtin_header_t *h = Blob();
  char expect[32];
  if (!h) return 0;
  MakePath(h, expect);
  return strcmp(path, expect) == 0;
}

uint32_t ocd_builtin_size(void) {
  const builtin_header_t *h = Blob();
  return h ? h->size : 0;
}

size_t ocd_builtin_read(uint32_t offset, void *buffer, size_t len) {
  const builtin_header_t *builtin = Blob();
  if (!builtin || offset >= builtin->size) return 0;
  if (offset + len > builtin->size) len = builtin->size - offset;
  const uint8_t *base = (const uint8_t *)builtin;
  const uint32_t cs = builtin->chunk_size;
  uint8_t *dst = buffer;
  size_t done = 0;
  while (done < len) {
    uint32_t pos = offset + done;
    int32_t chunk = pos / cs;
    if (chunk + 1 != cached_chunk_plus1) {
      uint32_t start = builtin->offset[chunk], end = builtin->offset[chunk + 1];
      uint32_t want = (chunk == (int32_t)builtin->num_chunks - 1) ? builtin->size - chunk * cs : cs;
      cached_chunk_plus1 = 0;
      if (Inflate(base + start, end - start, chunk_buf, want) != (int)want) return done;
      cached_chunk_plus1 = chunk + 1;
    }
    uint32_t in_chunk = pos - chunk * cs;
    uint32_t n = cs - in_chunk;
    if (n > len - done) n = len - done;
    memcpy(dst + done, chunk_buf + in_chunk, n);
    done += n;
  }
  return done;
}
