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
