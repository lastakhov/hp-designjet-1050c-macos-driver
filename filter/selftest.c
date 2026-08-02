/*
 * Copyright (C) 2026 Leonid Astakhov
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Round-trip tests for the HP RTL raster encoders.
 *
 * The filter's encoders are static, so this pulls the whole translation
 * unit in and renames its main() out of the way rather than duplicating
 * or exporting them. The decoders below are written independently from
 * the spec (HP-GL/2 and HP RTL Reference Guide, "Compressing Data") so a
 * shared misreading cannot make a broken encoder look correct.
 *
 * Run with: make test
 */

#define main rastertohpgl2rtl_main
#include "rastertohpgl2rtl.c"
#undef main

#include <assert.h>

static int failures;

static void
check(const char *what, int ok)
{
  if (!ok)
  {
    printf("  FAIL: %s\n", what);
    failures++;
  }
}

/* --- independent decoders ------------------------------------------- */

static size_t
packbits_decode(const unsigned char *src, size_t n, unsigned char *dst)
{
  size_t i = 0, o = 0;
  while (i < n)
  {
    int c = (signed char)src[i++];
    if (c >= 0)
    {
      for (int k = 0; k <= c; k++)
        dst[o++] = src[i++];
    }
    else if (c != -128)
    {
      unsigned char b = src[i++];
      for (int k = 0; k <= -c; k++)
        dst[o++] = b;
    }
  }
  return o;
}

static void
delta_decode(const unsigned char *src, size_t n, const unsigned char *seed,
             unsigned char *dst, size_t rowlen)
{
  memcpy(dst, seed, rowlen);
  size_t i = 0, untreated = 0;
  while (i < n)
  {
    unsigned char cmd = src[i++];
    size_t cnt = (cmd >> 5) + 1;
    size_t off = cmd & 31;
    if (off == 31)
    {
      for (;;)
      {
        unsigned char b = src[i++];
        off += b;
        if (b != 255)
          break;
      }
    }
    size_t pos = untreated + off;
    for (size_t k = 0; k < cnt; k++)
      dst[pos + k] = src[i++];
    untreated = pos + cnt;
  }
}

/* --- test drivers ---------------------------------------------------- */

static void
test_packbits(const char *name, const unsigned char *row, size_t len)
{
  unsigned char *enc = malloc(2 * len + 64);
  unsigned char *dec = malloc(len + 64);
  size_t n = packbits_encode(row, len, enc);
  size_t got = packbits_decode(enc, n, dec);

  char label[128];
  snprintf(label, sizeof(label), "packbits/%s length", name);
  check(label, got == len);
  snprintf(label, sizeof(label), "packbits/%s content", name);
  check(label, memcmp(row, dec, len) == 0);

  free(enc);
  free(dec);
}

static void
test_delta(const char *name, const unsigned char *row,
           const unsigned char *seed, size_t len)
{
  unsigned char *enc = malloc(2 * len + 64);
  unsigned char *dec = malloc(len + 64);
  size_t n = delta_encode(row, seed, len, enc);
  delta_decode(enc, n, seed, dec, len);

  char label[128];
  snprintf(label, sizeof(label), "delta/%s", name);
  check(label, memcmp(row, dec, len) == 0);

  free(enc);
  free(dec);
}

static void
both(const char *name, const unsigned char *row, const unsigned char *seed,
     size_t len)
{
  test_packbits(name, row, len);
  test_delta(name, row, seed, len);
}

int
main(void)
{
  const size_t LEN = 4099; /* deliberately not a multiple of 8 or 128 */
  unsigned char *row = malloc(LEN);
  unsigned char *seed = malloc(LEN);

  printf("HP RTL encoder round-trip tests (row = %zu bytes)\n", LEN);

  /* 1. all zeros against a zero seed - the degenerate "nothing to do" case */
  memset(row, 0, LEN);
  memset(seed, 0, LEN);
  both("all-zero", row, seed, LEN);
  check("delta/all-zero encodes to nothing",
        delta_encode(row, seed, LEN, malloc(2 * LEN + 64)) == 0);

  /* 2. one long run - packbits' best case */
  memset(row, 0xAB, LEN);
  memset(seed, 0, LEN);
  both("single-run", row, seed, LEN);

  /* 3. incompressible noise - packbits' worst case, must not corrupt */
  for (size_t i = 0; i < LEN; i++)
    row[i] = (unsigned char)(i * 2654435761u >> 13);
  memset(seed, 0, LEN);
  both("pseudorandom", row, seed, LEN);

  /* 4. alternating bytes - defeats run detection entirely */
  for (size_t i = 0; i < LEN; i++)
    row[i] = (i & 1) ? 0x00 : 0xFF;
  memset(seed, 0, LEN);
  both("alternating", row, seed, LEN);

  /* 5. delta against an identical seed - must encode to zero bytes */
  memcpy(seed, row, LEN);
  check("delta/identical encodes to nothing",
        delta_encode(row, seed, LEN, malloc(2 * LEN + 64)) == 0);
  test_delta("identical", row, seed, LEN);

  /* 6. a single changed byte far into the row: exercises the multi-byte
   *    offset escape (offset 31 then repeated 255s), the most intricate
   *    corner of the delta format and the easiest to get wrong. */
  memset(seed, 0x11, LEN);
  memcpy(row, seed, LEN);
  row[4000] = 0x99;
  test_delta("far-single-change", row, seed, LEN);

  /* 7. changes at offsets that land exactly on the escape boundaries */
  for (size_t off = 28; off <= 34; off++)
  {
    memset(seed, 0x22, LEN);
    memcpy(row, seed, LEN);
    row[off] = 0x77;
    char name[64];
    snprintf(name, sizeof(name), "boundary-offset-%zu", off);
    test_delta(name, row, seed, LEN);
  }

  /* 8. exactly 8 and exactly 9 consecutive changes: the run field tops
   *    out at 8, so 9 must split into two commands. */
  for (size_t runlen = 1; runlen <= 9; runlen++)
  {
    memset(seed, 0x33, LEN);
    memcpy(row, seed, LEN);
    for (size_t k = 0; k < runlen; k++)
      row[100 + k] = (unsigned char)(0xC0 + k);
    char name[64];
    snprintf(name, sizeof(name), "run-of-%zu", runlen);
    test_delta(name, row, seed, LEN);
  }

  /* 9. every byte differs - delta's worst case, checks the size bound
   *    the caller allocates against (2*len + 64). */
  memset(seed, 0x00, LEN);
  memset(row, 0xFF, LEN);
  unsigned char *big = malloc(2 * LEN + 64);
  size_t n = delta_encode(row, seed, LEN, big);
  check("delta/all-different stays inside 2*len+64", n <= 2 * LEN + 64);
  test_delta("all-different", row, seed, LEN);
  free(big);

  /* 10. gamma table: identity at 1.0, monotonic and lightening above it */
  build_gamma_lut(1.0);
  int identity = 1;
  for (int i = 0; i < 256; i++)
    if (gamma_lut[i] != i)
      identity = 0;
  check("gamma 1.0 is identity", identity);
  check("gamma 1.0 marked inactive", gamma_active == 0);

  build_gamma_lut(2.5);
  int monotonic = 1, lightens = 1;
  for (int i = 1; i < 256; i++)
    if (gamma_lut[i] < gamma_lut[i - 1])
      monotonic = 0;
  for (int i = 1; i < 255; i++)
    if (gamma_lut[i] < i)
      lightens = 0;
  check("gamma 2.5 monotonic", monotonic);
  check("gamma 2.5 lightens midtones", lightens);
  check("gamma 2.5 keeps black at 0", gamma_lut[0] == 0);
  check("gamma 2.5 keeps white at 255", gamma_lut[255] == 255);

  build_gamma_lut(0.6);
  int darkens = 1;
  for (int i = 1; i < 255; i++)
    if (gamma_lut[i] > i)
      darkens = 0;
  check("gamma 0.6 darkens midtones", darkens);

  /* 11. colour separation. Plane order is K, C, M, Y - fixed by the Simple
   *     Colour KCMY palette, whose index bits run black, cyan, magenta,
   *     yellow upwards from the least significant, which is also the order
   *     the spec requires them to be sent in. */
  {
    const unsigned W = 6;
    unsigned char rgb[6 * 3] = {
        255, 255, 255, /* white  - no ink at all */
        0,   0,   0,   /* black  - K only, no CMY */
        255, 0,   0,   /* red    - M + Y */
        0,   255, 0,   /* green  - C + Y */
        0,   0,   255, /* blue   - C + M */
        128, 128, 128, /* grey   - K only */
    };
    unsigned char k[6], c[6], m[6], yl[6];
    unsigned char *planes[4] = {k, c, m, yl};
    separate_row(rgb, W, MODE_KCMY, planes);

    check("white uses no ink",
          k[0] == 0 && c[0] == 0 && m[0] == 0 && yl[0] == 0);
    check("black is pure K, no CMY",
          k[1] == 255 && c[1] == 0 && m[1] == 0 && yl[1] == 0);
    check("grey is pure K, no CMY",
          k[5] == 127 && c[5] == 0 && m[5] == 0 && yl[5] == 0);
    check("red is M+Y with no K", k[2] == 0 && c[2] == 0 && m[2] == 255 &&
                                      yl[2] == 255);
    check("green is C+Y with no K", k[3] == 0 && c[3] == 255 && m[3] == 0 &&
                                        yl[3] == 255);
    check("blue is C+M with no K", k[4] == 0 && c[4] == 255 && m[4] == 255 &&
                                       yl[4] == 0);

    /* Grey component replacement must never leave a channel able to
     * recreate the neutral: after GCR at least one of C/M/Y is zero. */
    int gcr_ok = 1;
    for (unsigned x = 0; x < W; x++)
      if (c[x] && m[x] && yl[x])
        gcr_ok = 0;
    check("GCR always empties at least one chromatic channel", gcr_ok);

    separate_row(rgb, W, MODE_GRAY_K, planes);
    check("gray mode: white is blank", k[0] == 0);
    check("gray mode: black is full", k[1] == 255);
    black_start = 0;
  }

  /* 11b. partial black generation. The whole point of raising the black
   *      start point rather than scaling K down is that pure black must
   *      stay pure black ink at every setting - otherwise backing GCR off
   *      for photographs would quietly put coloured ink back under text. */
  {
    const unsigned W = 4;
    unsigned char rgb[4 * 3] = {
        0,   0,   0,   /* pure black */
        128, 128, 128, /* mid grey   */
        64,  64,  64,  /* dark grey  */
        255, 255, 255, /* white      */
    };
    unsigned char k[4], c[4], m[4], yl[4];
    unsigned char *planes[4] = {k, c, m, yl};

    int starts[] = {0, 64, 128, 192};
    for (unsigned si = 0; si < sizeof(starts) / sizeof(starts[0]); si++)
    {
      black_start = starts[si];
      separate_row(rgb, W, MODE_KCMY, planes);

      char label[96];
      snprintf(label, sizeof(label),
               "black start %d: pure black is K=255 with no CMY", starts[si]);
      check(label, k[0] == 255 && c[0] == 0 && m[0] == 0 && yl[0] == 0);

      snprintf(label, sizeof(label), "black start %d: white stays blank",
               starts[si]);
      check(label, k[3] == 0 && c[3] == 0 && m[3] == 0 && yl[3] == 0);

      /* Whatever K is withheld must show up as neutral CMY instead, so
       * the tone is still reproduced - just not with black ink. */
      snprintf(label, sizeof(label),
               "black start %d: mid grey keeps its total density",
               starts[si]);
      check(label, k[1] + c[1] == 127 && c[1] == m[1] && m[1] == yl[1]);
    }

    /* Raising the start point must not *increase* black anywhere. */
    unsigned char kk[4][4];
    for (int si = 0; si < 4; si++)
    {
      black_start = starts[si];
      separate_row(rgb, W, MODE_KCMY, planes);
      memcpy(kk[si], k, 4);
    }
    int monotonic = 1;
    for (int si = 1; si < 4; si++)
      for (int x = 0; x < 4; x++)
        if (kk[si][x] > kk[si - 1][x])
          monotonic = 0;
    check("raising the black start never adds black ink", monotonic);
    check("mid grey loses black as the start rises", kk[3][1] < kk[0][1]);

    black_start = 0;
  }

  /* 12. dithering: bit packing is MSB-first, and flat input reproduces the
   *     right average ink coverage rather than drifting. */
  {
    const unsigned W = 64;
    size_t pb = (W + 7) / 8;
    unsigned char inkrow[64], bitrow[8];
    int *ec = calloc(W + 2, sizeof(int));
    int *en = calloc(W + 2, sizeof(int));

    memset(inkrow, 255, W); /* solid ink */
    dither_plane(inkrow, W, ec, en, bitrow, pb);
    int allset = 1;
    for (size_t i = 0; i < pb; i++)
      if (bitrow[i] != 0xFF)
        allset = 0;
    check("solid ink sets every bit", allset);

    memset(ec, 0, (W + 2) * sizeof(int));
    memset(inkrow, 0, W); /* no ink */
    dither_plane(inkrow, W, ec, en, bitrow, pb);
    int allclear = 1;
    for (size_t i = 0; i < pb; i++)
      if (bitrow[i] != 0x00)
        allclear = 0;
    check("no ink clears every bit", allclear);

    /* Leftmost pixel must land in bit 7 of byte 0. */
    memset(ec, 0, (W + 2) * sizeof(int));
    memset(inkrow, 0, W);
    inkrow[0] = 255;
    dither_plane(inkrow, W, ec, en, bitrow, pb);
    check("pixel 0 is the MSB of byte 0", (bitrow[0] & 0x80) != 0);

    /* 50% grey should come out near 50% coverage, not 0 or 100. */
    memset(ec, 0, (W + 2) * sizeof(int));
    memset(en, 0, (W + 2) * sizeof(int));
    int lit = 0;
    for (int row = 0; row < 32; row++)
    {
      memset(inkrow, 128, W);
      dither_plane(inkrow, W, ec, en, bitrow, pb);
      int *t = ec; ec = en; en = t;
      for (size_t i = 0; i < pb; i++)
        for (int b = 0; b < 8; b++)
          if (bitrow[i] & (0x80 >> b))
            lit++;
    }
    double coverage = lit / (double)(W * 32);
    check("50% ink dithers to roughly 50% coverage",
          coverage > 0.45 && coverage < 0.55);

    free(ec);
    free(en);
  }

  free(row);
  free(seed);

  if (failures)
  {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}
