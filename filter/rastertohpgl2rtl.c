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
 * rastertohpgl2rtl - CUPS raster filter for HP DesignJet 1050C.
 *
 * Converts CUPS Raster (24-bit RGB, chunked) into a print-ready stream for
 * a plotter that speaks only PJL + HP-GL/2 + HP RTL (no PCL, no PostScript):
 * confirmed for this exact unit via SNMP (hpicPrinter MIB):
 *   COMMAND SET: PML,MLC,PJL,HP-GL,HP-GL/2,RTL
 *
 * Wire format per page, per the HP-GL/2 and HP RTL Reference Guide
 * (HP p/n 5961-3526), "Example of 24-bit RGB Data with Scaling" /
 * "Color Raster" examples:
 *
 *   ESC E                       Reset
 *   ESC % 0 B                   Enter HP-GL/2 mode
 *   BP5,1;IN;[QL n;]PS w,h;TR0;  Begin plot (no autorotate), init,
 *                               optional quality level, plot size
 *   ESC % 0 A                   Enter RTL mode (CAP = previous RTL CAP,
 *                               i.e. (0,0) right after Reset - see the
 *                               note by emit_page() on why this is 0 and
 *                               not the spec's suggested 1)
 *   ESC * v 1 N                 Source transparency off
 *   ESC * v 1 O                 Pattern transparency off
 *   ESC & a 1 N                 Negative motion off (print on the fly,
 *                               swath by swath, instead of buffering the
 *                               whole page - see the note by emit_page())
 *   ESC * r # S                 Source raster width (pixels)
 *   ESC * t # R                 Graphics resolution (dpi)
 *   ESC * r -4 U                Simple Colour KCMY palette, in the default
 *                               separated modes - or, in legacy RGB mode,
 *                               ESC * v 6 W [0,3,3,8,8,8] (Configure Image
 *                               Data, direct-by-pixel RGB888) instead
 *   { ESC * r 1 A                 Start raster graphics at CAP (unscaled)
 *     { ESC * b # M               Compression method, emitted only when it
 *                                 changes: 0 (none), 2 (Packbits) or 3
 *                                 (delta-row), chosen per plane by
 *                                 whichever comes out smallest
 *       ESC * b # V <data>        Planes K, C, M of this row...
 *       ESC * b # W <data> }*     ...then Y, which advances the row.
 *                                 (RGB mode sends the chunky row as a
 *                                 single W transfer.)
 *     ESC * r C                   End raster graphics }*
 *                               One block per page by default. The braces
 *                               repeat only if banding is switched back on
 *                               - see band_target on why it is off.
 *   ESC % 0 B                   Back to HP-GL/2
 *   PG;                         Advance/print the page
 *
 * The whole job is wrapped once in PJL:
 *   ESC % -12345 X @PJL JOB ...  @PJL ENTER LANGUAGE=HPGL2
 *   ... pages ...
 *   ESC E  ESC % -12345 X @PJL EOJ ...
 *
 * Delivery: normally a CUPS filter just writes to stdout and lets the
 * queue's backend (socket://host:9100) deliver it. Apple's stock socket
 * backend does that with small, unbuffered writes, and on this printer's
 * old JetDirect TCP stack that triggers the classic Nagle-vs-delayed-ACK
 * stall (every small packet waits ~200ms for the peer's ACK before the
 * next one goes out) - print jobs were taking many times longer than the
 * raw byte count justified. Since the stock backend lives on the
 * SIP-sealed system volume and can't be modified or replaced, this filter
 * instead opens its own TCP connection straight to the printer (host:port
 * parsed from the $DEVICE_URI CUPS sets for the job) with TCP_NODELAY, and
 * writes everything there directly. The queue's backend is left as the
 * harmless no-op "file:/dev/null" backend, which drains a normally-empty
 * stdout. If the direct connection cannot be made for any reason, this
 * falls back to plain stdout so a socket:// queue still works.
 */

#include <cups/raster.h>
#include <cups/cups.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <errno.h>

#define HPGL_UNITS_PER_INCH 1016.0

/*
 * Bytes of raster per Start/End Raster Graphics block, or 0 - the default -
 * for one block covering the whole page, which is the form HP's own worked
 * examples use.
 *
 * Splitting was added on the belief that the printer was running out of
 * memory and dropping the bottom of large images. That diagnosis was
 * wrong: the bottom was being lost to a swapped PS length/width parameter.
 * Worse, the splitting turned out to cause a defect of its own - End
 * Raster Graphics moves the CAP and, per the spec, "fills the area through
 * which the CAP moves with zeros", and those boundaries showed up as
 * visible seams across flat tone, spaced exactly one band apart (17.4 mm
 * on A3 at 300 dpi). Confirmed by printing the same flat grey page either
 * way: banded had seams, single-block did not.
 *
 * Memory is in any case already handled by ESC&a1N, which tells the device
 * it may interleave parsing and printing rather than composing the whole
 * page first. Splitting is kept only as an escape hatch for jobs large
 * enough that this turns out not to be enough.
 */
static size_t band_target;

/*
 * HP-GL/2 quality level, 0 (draft) to 100 (best), or -1 to send nothing and
 * leave the device on whatever its front panel says.
 *
 * This is the print-quality control, and it lives in HP-GL/2 rather than
 * PJL - which is why probing the device over PJL turned up nothing and led
 * to the wrong conclusion that quality could not be driven from the host at
 * all. HP's own Windows driver sends QL51 in the picture header. Per the
 * spec a device "might vary paper speed, resolution, or rasterization
 * algorithms" in response, which is exactly the multi-pass behaviour that
 * governs how much banding shows.
 *
 * The instruction only takes effect in the picture header state - "you
 * cannot change quality levels in the picture body state" - so it has to go
 * out with IN and PS, before any drawing.
 */
static int quality_level = -1;

/*
 * HP RTL render algorithm (ESC*t#J), or -1 to leave the device on its own
 * default. Values from the spec: 0 device-best, 3/11 pattern dither,
 * 7 cluster-ordered, 13 scatter dither, and monochrome variants.
 *
 * This only applies when the printer is doing the screening, i.e. in RGB
 * mode. HP's Windows driver sends 13.
 */
static int render_algorithm = -1;

static FILE *out; /* Where printer bytes go: direct socket, or stdout. */

/* Gamma correction lookup table, applied to every RGB byte before
 * compression. Identity unless the job asks for a non-1.0 gamma. */
static unsigned char gamma_lut[256];
static int gamma_active;

/*
 * Build the gamma table using the transfer function the HP-GL/2 and HP RTL
 * Reference Guide specifies for this printer family (p.401):
 *
 *   new = ((old / 255) ^ (1 / gamma)) * 255 + 0.5
 *
 * Higher gamma lightens the image, so less ink is laid down; lower gamma
 * darkens it and lays down more. The guide notes ~2.5 is typical for a
 * DesignJet on HP Special Paper. Note this is a driver-side correction:
 * if a device also applies its own Gamma Correction command (ESC*t#I),
 * doing both would double-correct - we never send that command.
 */
static void
build_gamma_lut(double gamma)
{
  if (gamma <= 0.0 || fabs(gamma - 1.0) < 1e-6)
  {
    for (int i = 0; i < 256; i++)
      gamma_lut[i] = (unsigned char)i;
    gamma_active = 0;
    return;
  }

  for (int i = 0; i < 256; i++)
  {
    double v = pow(i / 255.0, 1.0 / gamma) * 255.0 + 0.5;
    if (v < 0.0)
      v = 0.0;
    if (v > 255.0)
      v = 255.0;
    gamma_lut[i] = (unsigned char)v;
  }
  gamma_active = 1;
}

static void
sanitize_title(const char *in, char *out_str, size_t outlen)
{
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j + 1 < outlen; i++)
  {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\' || c < 0x20)
      continue;
    out_str[j++] = (char)c;
  }
  out_str[j] = '\0';
  if (j == 0)
    strncpy(out_str, "Untitled", outlen - 1);
}

/*
 * TIFF Packbits encode. dst must be at least srclen + ceil(srclen/128) + 1
 * bytes. Returns encoded length.
 */
static size_t
packbits_encode(const unsigned char *src, size_t srclen, unsigned char *dst)
{
  size_t i = 0, o = 0;

  while (i < srclen)
  {
    /* Look for a run of identical bytes. */
    size_t run = 1;
    while (i + run < srclen && run < 128 && src[i + run] == src[i])
      run++;

    if (run >= 2)
    {
      dst[o++] = (unsigned char)(-(long)(run - 1)); /* control: -(run-1) */
      dst[o++] = src[i];
      i += run;
      continue;
    }

    /* Collect a literal span until the next worthwhile run. */
    size_t start = i;
    size_t len = 0;
    while (i < srclen && len < 128)
    {
      size_t r = 1;
      while (i + r < srclen && r < 128 && src[i + r] == src[i])
        r++;
      if (r >= 2)
        break;
      i++;
      len++;
    }
    dst[o++] = (unsigned char)(len - 1); /* control: len-1, 0..127 */
    memcpy(dst + o, src + start, len);
    o += len;
  }
  return o;
}

/*
 * Seed-row (delta-row) encode - HP RTL compression method 3. Encodes only
 * the bytes that differ from the previous row ("seed row"), as a series of
 *
 *   <command byte> [<extra offset bytes>] <1..8 replacement bytes>
 *
 * where the command byte packs the replacement count (top 3 bits, 0..7
 * meaning 1..8 bytes) and the offset from the last untreated byte (low 5
 * bits). A low-5-bits value of 31 means further offset bytes follow, each
 * 255 meaning "add 255 and read another", terminated by a byte < 255.
 *
 * This is dramatically better than Packbits on scaled-up photographic
 * content, where vertically adjacent rows are nearly identical (measured
 * 4.3x vs 1.5x on a real 600dpi photo job), and dramatically worse on
 * blank/flat areas, where Packbits' runs win instead - hence the per-row
 * method selection in emit_page().
 *
 * dst needs room for the worst case, 2*srclen + 64. Returns encoded length.
 */
static size_t
delta_encode(const unsigned char *src, const unsigned char *seed,
             size_t srclen, unsigned char *dst)
{
  size_t i = 0, o = 0, untreated = 0;

  while (i < srclen)
  {
    if (src[i] == seed[i])
    {
      i++;
      continue;
    }

    /* Gather up to 8 consecutive differing bytes. */
    size_t run = 0;
    while (i + run < srclen && run < 8 && src[i + run] != seed[i + run])
      run++;

    size_t offset = i - untreated;
    unsigned char cmd = (unsigned char)((run - 1) << 5);

    if (offset < 31)
    {
      dst[o++] = cmd | (unsigned char)offset;
    }
    else
    {
      dst[o++] = cmd | 31;
      size_t rem = offset - 31;
      while (rem >= 255)
      {
        dst[o++] = 255;
        rem -= 255;
      }
      dst[o++] = (unsigned char)rem;
    }

    memcpy(dst + o, src + i, run);
    o += run;
    i += run;
    untreated = i;
  }
  return o;
}

static FILE *
debug_log_open(void)
{
  /* macOS's cupsd runs filters under restricted privileges/sandboxing,
   * so a hardcoded /tmp path is not reliably writable - try TMPDIR (the
   * per-job temp dir CUPS hands to filters) and a few other candidates. */
  static const char *candidates[] = {
      "/private/tmp/rastertohpgl2rtl_debug.log",
      "/tmp/rastertohpgl2rtl_debug.log",
      "/private/var/spool/cups/tmp/rastertohpgl2rtl_debug.log",
      "/Library/Printers/HPDesignJet1050C/debug.log",
  };
  const char *tmpdir = getenv("TMPDIR");
  char tmpdir_path[512];
  if (tmpdir && *tmpdir)
  {
    snprintf(tmpdir_path, sizeof(tmpdir_path),
             "%s/rastertohpgl2rtl_debug.log", tmpdir);
    FILE *f = fopen(tmpdir_path, "a");
    if (f)
      return f;
  }
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
  {
    FILE *f = fopen(candidates[i], "a");
    if (f)
      return f;
  }
  return NULL;
}

/* Every debug line goes to a log file (best-effort - see debug_log_open)
 * AND to stderr, which CUPS always captures into its own logs regardless
 * of filesystem sandboxing. Tagged "DEBUG: rastertohpgl2rtl:" so it is
 * easy to grep for in /private/var/log/cups/error_log even when the log
 * file above never works out. */
#define DBG(...)                                                          \
  do                                                                      \
  {                                                                       \
    FILE *_dbg = debug_log_open();                                       \
    if (_dbg)                                                            \
    {                                                                    \
      fprintf(_dbg, __VA_ARGS__);                                        \
      fclose(_dbg);                                                      \
    }                                                                     \
    fprintf(stderr, "DEBUG: rastertohpgl2rtl: " __VA_ARGS__);            \
  } while (0)

static double
now_seconds(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/*
 * Parse host[:port] out of a CUPS device-uri of the form
 * "socket://host:port" (the port is optional and defaults to 9100, the
 * standard JetDirect/AppSocket port). Returns 1 and fills in host and
 * port on success, 0 if the URI doesn't look like a socket:// URI we can
 * parse.
 */
static int
parse_socket_uri(const char *uri, char *host, size_t hostlen, int *port)
{
  if (!uri || strncmp(uri, "socket://", 9) != 0)
    return 0;

  const char *p = uri + 9;
  const char *end = p + strcspn(p, "/?");
  const char *colon = memchr(p, ':', (size_t)(end - p));

  size_t n = colon ? (size_t)(colon - p) : (size_t)(end - p);
  if (n == 0 || n >= hostlen)
    return 0;

  memcpy(host, p, n);
  host[n] = '\0';
  *port = colon ? atoi(colon + 1) : 9100;
  if (*port <= 0 || *port > 65535)
    *port = 9100;
  return 1;
}

/*
 * Connect directly to the printer with TCP_NODELAY set, bypassing the
 * stock CUPS socket backend's slow, small-write I/O pattern (see the
 * file header comment). Returns a FILE* opened for writing on success,
 * or NULL if anything about this fails - callers should fall back to
 * stdout in that case, since the job must still go out somehow.
 */
static FILE *
connect_direct(void)
{
  const char *uri = getenv("DEVICE_URI");
  char host[256];
  int port;

  if (!parse_socket_uri(uri, host, sizeof(host), &port))
  {
    DBG("connect_direct: no usable DEVICE_URI (\"%s\"), falling back to "
        "stdout\n",
        uri ? uri : "(null)");
    return NULL;
  }

  struct addrinfo hints, *res = NULL, *rp;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  char portstr[16];
  snprintf(portstr, sizeof(portstr), "%d", port);

  int gai_err = getaddrinfo(host, portstr, &hints, &res);
  if (gai_err != 0)
  {
    DBG("connect_direct: getaddrinfo(%s:%d) failed: %s\n", host, port,
        gai_strerror(gai_err));
    return NULL;
  }

  int fd = -1;
  for (rp = res; rp; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0)
      continue;
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd < 0)
  {
    DBG("connect_direct: connect(%s:%d) failed: %s\n", host, port,
        strerror(errno));
    return NULL;
  }

  int one = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
  {
    DBG("connect_direct: setsockopt(TCP_NODELAY) failed: %s (continuing "
        "anyway)\n",
        strerror(errno));
  }

  FILE *f = fdopen(fd, "w");
  if (!f)
  {
    DBG("connect_direct: fdopen failed: %s\n", strerror(errno));
    close(fd);
    return NULL;
  }

  /* Large buffer: coalesce our many small printf()s into big write()s -
   * the whole point is to avoid dribbling small packets onto the wire. */
  static char iobuf[262144];
  setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

  DBG("connect_direct: connected to %s:%d with TCP_NODELAY\n", host, port);
  return f;
}

static void
emit_job_header(const char *title, int resolution, unsigned paper_width_dp,
                 unsigned paper_length_dp)
{
  char safe_title[256];
  sanitize_title(title, safe_title, sizeof(safe_title));

  /* Field set and order mirror the reference guide's own worked example
   * ("Example Showing the Structure of a PJL Job", HP-GL/2 and HP RTL
   * Reference Guide, p.395) as closely as possible - notably PAPERWIDTH/
   * PAPERLENGTH/ORIENTATION/PALETTESOURCE, which our first attempt
   * omitted and which left the device falling back to whatever media/
   * palette source was set on the front panel instead of what we asked
   * for, producing a blank page. */
  fprintf(out, "\033%%-12345X@PJL JOB NAME=\"%s\"\r\n", safe_title);
  fprintf(out, "@PJL SET RESOLUTION=%d\r\n", resolution);
  fprintf(out, "@PJL SET RENDERMODE=COLOR\r\n");
  fprintf(out, "@PJL SET MIRROR=OFF\r\n");
  /* SMALLER, not NORMAL: PJL on this device offers both, and NORMAL is
   * the wider of the two - asking for it was silently costing usable
   * page area on every job regardless of what margins the user set in
   * the print dialog. */
  fprintf(out, "@PJL SET MARGINS=SMALLER\r\n");
  fprintf(out, "@PJL SET PALETTESOURCE=SOFTWARE\r\n");
  fprintf(out, "@PJL SET PAPERLENGTH=%u\r\n", paper_length_dp);
  fprintf(out, "@PJL SET PAPERWIDTH=%u\r\n", paper_width_dp);
  /* Always PORTRAIT, which for PJL means "do not rotate what I send you"
   * - it is not a statement about the shape of the page. CUPS hands us a
   * raster that is already laid out exactly as it should appear on the
   * media, with any landscape rotation already applied upstream by the
   * rasterizer, so asking the printer to rotate as well turns a correct
   * landscape page sideways. On this plotter landscape is a property of
   * how the operator feeds the sheet (wide edge first), not something
   * the driver should be rotating the image for. */
  fprintf(out, "@PJL SET ORIENTATION=PORTRAIT\r\n");
  fprintf(out, "@PJL ENTER LANGUAGE=HPGL2\r\n");

  DBG("emit_job_header: resolution=%d paper_w_dp=%u paper_l_dp=%u "
      "title=\"%s\"\n",
      resolution, paper_width_dp, paper_length_dp, safe_title);
}

static void
emit_job_trailer(const char *title)
{
  char safe_title[256];
  sanitize_title(title, safe_title, sizeof(safe_title));

  fprintf(out, "\033E");
  fprintf(out, "\033%%-12345X@PJL EOJ NAME=\"%s\"\r\n", safe_title);
}

/*
 * Colour handling modes.
 *
 * MODE_RGB sends 24-bit RGB and lets the printer work out how to lay ink
 * down. That is simple, but this device composites black out of cyan,
 * magenta and yellow rather than reaching for the black cartridge, so
 * black text and line work come out muddy, use three times the ink, and
 * show a colour cast.
 *
 * MODE_KCMY does the separation here instead: convert to CMY, pull the
 * common grey component out into a real K channel, dither each channel to
 * one bit, and hand the printer four 1-bit planes through the Simple
 * Colour KCMY palette. Black then prints as black ink. It is also far
 * less data - 4 bits per pixel instead of 24 before compression.
 *
 * MODE_GRAY_K is the same machinery with a single K plane, for line work
 * that has no colour in it at all.
 */
enum color_mode
{
  MODE_RGB = 0,
  MODE_KCMY,
  MODE_GRAY_K
};

static enum color_mode color_mode = MODE_KCMY;

/* 0 = ordered (Bayer), 1 = Floyd-Steinberg. See dither_plane_ordered()
 * for why ordered is the default despite being the grainier of the two. */
static int dither_diffuse;

/*
 * Black start point for grey component replacement, 0..254.
 *
 * Grey component replacement takes whatever cyan, magenta and yellow have
 * in common and moves it into the black channel. Doing that in full (start
 * = 0) is what stops neutrals being mixed out of coloured inks, and it is
 * exactly right for text and line work. On photographs it can flatten the
 * shadows, because everything neutral becomes black ink and nothing else.
 *
 * Backing it off is the usual remedy, but the naive way to do that -
 * scaling K by some factor below 1 - would put coloured ink back underneath
 * pure black text, undoing the very problem this was written to fix. So
 * instead of scaling, this raises the point at which black *starts*:
 *
 *   K = (min(c,m,y) - start) / (255 - start), clamped at zero
 *
 * Light and mid neutrals below the start point are left to CMY, which keeps
 * photographic shadows from going flat, while anything fully saturated
 * still maps to K = 255 whatever the setting - so black stays pure black at
 * every level.
 */
static int black_start;

/* Number of 1-bit planes each mode sends per row (0 = chunky RGB). */
static int
planes_for_mode(enum color_mode m)
{
  switch (m)
  {
    case MODE_KCMY:
      return 4;
    case MODE_GRAY_K:
      return 1;
    default:
      return 0;
  }
}

/*
 * Separate one row of RGB into per-channel ink amounts, 0 = no ink and
 * 255 = full ink.
 *
 * Plane order is fixed by the Simple Colour KCMY palette, whose index bits
 * run black, cyan, magenta, yellow from least significant upwards - and
 * the spec requires the least significant plane to be sent first, so
 * ink[0]=K, [1]=C, [2]=M, [3]=Y is also the order they go on the wire.
 *
 * How much of the common grey component becomes K is governed by
 * black_start - see its comment. Whatever K is generated is removed from
 * all three chromatic channels, so they never re-create what black is
 * already printing.
 */
static void
separate_row(const unsigned char *rgb, unsigned width, enum color_mode mode,
             unsigned char *ink[4])
{
  for (unsigned x = 0; x < width; x++)
  {
    int r = rgb[x * 3 + 0];
    int g = rgb[x * 3 + 1];
    int b = rgb[x * 3 + 2];

    if (mode == MODE_GRAY_K)
    {
      /* Rec. 601 luma, then invert: dark pixels want lots of black ink. */
      int luma = (r * 77 + g * 151 + b * 28) >> 8;
      ink[0][x] = (unsigned char)(255 - luma);
      continue;
    }

    int c = 255 - r;
    int m = 255 - g;
    int y = 255 - b;

    int grey = c < m ? c : m;
    if (y < grey)
      grey = y;

    int k;
    if (black_start <= 0)
    {
      k = grey; /* full replacement */
    }
    else if (grey <= black_start)
    {
      k = 0; /* below the start point, leave the neutral to CMY */
    }
    else
    {
      /* Ramp from the start point up to full black at 255, so a fully
       * saturated neutral still lands on exactly K=255. */
      k = (grey - black_start) * 255 / (255 - black_start);
    }

    ink[0][x] = (unsigned char)k;
    ink[1][x] = (unsigned char)(c - k < 0 ? 0 : c - k);
    ink[2][x] = (unsigned char)(m - k < 0 ? 0 : m - k);
    ink[3][x] = (unsigned char)(y - k < 0 ? 0 : y - k);
  }
}

/* Bayer 8x8 ordered-dither thresholds, scaled to 0..255. */
static const unsigned char bayer8[8][8] = {
    {  0, 128,  32, 160,   8, 136,  40, 168},
    {192,  64, 224,  96, 200,  72, 232, 104},
    { 48, 176,  16, 144,  56, 184,  24, 152},
    {240, 112, 208,  80, 248, 120, 216,  88},
    { 12, 140,  44, 172,   4, 132,  36, 164},
    {204,  76, 236, 108, 196,  68, 228, 100},
    { 60, 188,  28, 156,  52, 180,  20, 148},
    {252, 124, 220,  92, 244, 116, 212,  84},
};

/*
 * Ordered (Bayer) dither of one channel down to one bit.
 *
 * Chosen as the default over error diffusion specifically because of what
 * it does to compressibility. The threshold pattern repeats every 8
 * pixels, which is exactly one packed byte, so a flat area of the image
 * becomes one byte value repeated across the whole row - which Packbits
 * then collapses to almost nothing. Error diffusion turns the same flat
 * area into unrepeatable noise; measured on a full-page gradient it made
 * the job 27x larger than sending contone RGB and letting the printer
 * halftone. Ordered dither is grainier than error diffusion on
 * photographs, which is why the choice is exposed rather than hardcoded.
 */
static void
dither_plane_ordered(const unsigned char *ink, unsigned width, unsigned row,
                     unsigned char *bits, size_t planebytes)
{
  const unsigned char *thresh = bayer8[row & 7];
  memset(bits, 0, planebytes);

  for (unsigned x = 0; x < width; x++)
    if (ink[x] > thresh[x & 7])
      bits[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
}

/*
 * Floyd-Steinberg error diffusion of one channel down to one bit, packed
 * MSB-first (bit 7 of the first byte is the leftmost pixel, matching the
 * bit order HP RTL expects for unencoded row data).
 *
 * err_cur/err_next are indexed with a +1 bias so that spilling error onto
 * x-1 at the left edge lands in a scratch slot instead of out of bounds.
 * They persist across bands on purpose: bands are only protocol framing,
 * the image runs continuously through them, and resetting the diffusion
 * state at each boundary would leave a visible seam.
 */
static void
dither_plane(const unsigned char *ink, unsigned width, int *err_cur,
             int *err_next, unsigned char *bits, size_t planebytes)
{
  memset(bits, 0, planebytes);
  memset(err_next, 0, (width + 2) * sizeof(int));

  for (unsigned x = 0; x < width; x++)
  {
    int v = ink[x] + err_cur[x + 1];
    int on = v >= 128;
    int e = v - (on ? 255 : 0);

    if (on)
      bits[x >> 3] |= (unsigned char)(0x80 >> (x & 7));

    /* 7/16 right, 3/16 below-left, 5/16 below, 1/16 below-right. */
    err_cur[x + 2] += e * 7 / 16;
    err_next[x] += e * 3 / 16;
    err_next[x + 1] += e * 5 / 16;
    err_next[x + 2] += e * 1 / 16;
  }
}

static int
emit_page(cups_raster_t *ras, cups_page_header2_t *header, int page_num)
{
  unsigned width = header->cupsWidth;
  unsigned height = header->cupsHeight;
  unsigned bpl = header->cupsBytesPerLine;
  double xres = header->HWResolution[0] > 0 ? header->HWResolution[0] : 300;
  double yres = header->HWResolution[1] > 0 ? header->HWResolution[1] : 300;
  double t_start = now_seconds();
  size_t total_out = 0;

  if (header->cupsBitsPerColor != 8 ||
      !(header->cupsColorSpace == CUPS_CSPACE_RGB ||
        header->cupsColorSpace == CUPS_CSPACE_SRGB) ||
      header->cupsNumColors != 3)
  {
    DBG("page=%d ABORT unsupported raster format: colorspace=%d bpc=%d "
        "numcolors=%d (expected 8-bit RGB)\n",
        page_num, header->cupsColorSpace, header->cupsBitsPerColor,
        header->cupsNumColors);
    return 0;
  }

  if (width == 0 || height == 0 || bpl == 0)
  {
    DBG("page=%d ABORT empty page: %ux%u bpl=%u\n", page_num, width, height,
        bpl);
    return 0;
  }

  long plot_w = lround(width / xres * HPGL_UNITS_PER_INCH);
  long plot_h = lround(height / yres * HPGL_UNITS_PER_INCH);

  DBG("page=%d emit_page start: width=%u height=%u bpl=%u xres=%.1f "
      "yres=%.1f plot_w=%ld plot_h=%ld t=%.3f\n",
      page_num, width, height, bpl, xres, yres, plot_w, plot_h, t_start);

  /* Reset, enter HP-GL/2, set up the page, switch to RTL. */
  fprintf(out, "\033E");
  fprintf(out, "\033%%0B");
  /* PS takes *length first, then width* - "PS length[,width]", where per
   * the Reference Guide (p.259) "the length always corresponds to the
   * direction of the plot frame advance" (i.e. vertically down the page)
   * and "the width is always the horizontal direction". Passing these in
   * the intuitive width,height order instead sets the logical page - and
   * therefore the hard-clip limits - to a page as tall as the image is
   * wide, silently clipping everything below that. That was the cause of
   * the long-running "bottom of the page is cut off" bug: on an A3 photo
   * we were sending PS11476,10959, so the printer clipped at 11476 units
   * = 287mm down a 420mm page, losing the bottom 133mm - matching the
   * ~130mm measured on paper. It also explains the bogus ~130mm "hardware
   * bottom margin" found during margin calibration: that was never the
   * hardware, it was this clip. */
  fprintf(out, "BP5,1;IN;");
  if (quality_level >= 0)
    fprintf(out, "QL%d;", quality_level);
  fprintf(out, "PS%ld,%ld;TR0;", plot_h, plot_w);
  /* Enter RTL mode with parameter 0 ("use previous HP RTL CAP", which is
   * (0,0) right after the Reset above) rather than the spec's parameter
   * 1 ("use current HP-GL/2 pen position"). Confirmed by hardware test
   * on this exact unit: parameter 1 silently fails to switch context at
   * all (the raster block is then parsed as unrecognized HP-GL/2 data
   * and dropped, producing a blank ejected page), while parameter 0
   * works and lands at the same (0,0) origin we want anyway. */
  fprintf(out, "\033%%0A");
  fprintf(out, "\033*v1N");
  fprintf(out, "\033*v1O");
  /* Negative Motion disabled: the printer prints on the fly, as data
   * arrives, instead of composing the whole page in RAM first. This was
   * removed for a while because a naive stdout-through-the-stock-backend
   * pipeline made streaming look "slow" - but that slowness was actually
   * the Nagle/delayed-ACK stall fixed by connect_direct() above, not
   * on-the-fly printing itself. Buffering the complete page turned out to
   * have a real cost of its own on this printer's limited RAM: a
   * high-detail image (a photo, not the flat test shapes used during
   * development) can exceed available memory, and per the HP-GL/2 and
   * RTL Reference Guide's documented overflow behavior the device then
   * silently drops whatever didn't fit - which prints as the *bottom* of
   * the image missing, since data is sent top to bottom. On-the-fly mode
   * renders and discards each swath as it completes instead of holding
   * the whole decompressed image at once, avoiding that ceiling. We only
   * ever send raster top-to-bottom in one pass, so disabling negative
   * motion is safe - we never need to move the CAP backwards. */
  fprintf(out, "\033&a1N");
  fprintf(out, "\033*r%uS", width);
  fprintf(out, "\033*t%dR", (int)lround(xres));

  int nplanes = planes_for_mode(color_mode);
  size_t planebytes = (width + 7) / 8;
  /* Bytes actually handed to the compressors: one packed plane in the
   * separated modes, one chunky RGB row in the passthrough mode. */
  size_t unit = nplanes ? planebytes : bpl;

  if (nplanes)
  {
    /* Simple Colour replaces Configure Image Data entirely - it defines
     * its own fixed palette and forces indexed-planar encoding. It has to
     * be set before Start Raster Graphics, and is ignored inside raster
     * mode, so it goes here rather than per band. End Raster Graphics
     * does not reset the palette, so one setup covers every band. */
    fprintf(out, "\033*r%dU", nplanes == 4 ? -4 : 1);
  }
  else
  {
    /* Configure Image Data: color space=0, pixel encoding=3 (direct by
     * pixel), bits/index=3 (unused in direct mode), bits/R=G=B=8. */
    fprintf(out, "\033*v6W");
    unsigned char cfg[6] = {0, 3, 3, 8, 8, 8};
    fwrite(cfg, 1, sizeof(cfg), out);

    /* Pick the halftone the device uses on contone data. Only meaningful
     * in this mode, where the printer is the one screening the image -
     * in the separated modes we have already reduced everything to one
     * bit and there is nothing left for it to halftone. HP's own driver
     * sends 13 (scatter dither) here rather than relying on the device
     * default. */
    if (render_algorithm >= 0)
      fprintf(out, "\033*t%dJ", render_algorithm);
  }

  unsigned char *line = malloc(bpl);
  unsigned char *packed = malloc(unit + (unit / 128) + 16);
  unsigned char *delta = malloc(2 * unit + 64);
  /* One seed row per plane: the spec keeps a separate seed row for each
   * plane of a multi-plane image. */
  unsigned char *seed[4] = {NULL, NULL, NULL, NULL};
  unsigned char *ink[4] = {NULL, NULL, NULL, NULL};
  unsigned char *bits[4] = {NULL, NULL, NULL, NULL};
  int *err_cur[4] = {NULL, NULL, NULL, NULL};
  int *err_next[4] = {NULL, NULL, NULL, NULL};
  int alloc_ok = (line && packed && delta);

  for (int p = 0; p < (nplanes ? nplanes : 1); p++)
  {
    seed[p] = calloc(1, unit);
    if (!seed[p])
      alloc_ok = 0;
  }
  for (int p = 0; p < nplanes; p++)
  {
    ink[p] = malloc(width);
    bits[p] = malloc(planebytes);
    err_cur[p] = calloc(width + 2, sizeof(int));
    err_next[p] = calloc(width + 2, sizeof(int));
    if (!ink[p] || !bits[p] || !err_cur[p] || !err_next[p])
      alloc_ok = 0;
  }

#define FREE_PAGE_BUFFERS()                                               \
  do                                                                      \
  {                                                                       \
    free(line);                                                           \
    free(packed);                                                         \
    free(delta);                                                          \
    for (int _p = 0; _p < 4; _p++)                                        \
    {                                                                     \
      free(seed[_p]);                                                     \
      free(ink[_p]);                                                      \
      free(bits[_p]);                                                     \
      free(err_cur[_p]);                                                  \
      free(err_next[_p]);                                                 \
    }                                                                     \
  } while (0)

  if (!alloc_ok)
  {
    DBG("page=%d ABORT out of memory (bpl=%u unit=%zu planes=%d)\n",
        page_num, bpl, unit, nplanes);
    FREE_PAGE_BUFFERS();
    return 0;
  }

  DBG("page=%d color mode=%s planes=%d planebytes=%zu\n", page_num,
      color_mode == MODE_KCMY ? "KCMY"
                              : (color_mode == MODE_GRAY_K ? "K" : "RGB"),
      nplanes, planebytes);

  /* Band the raster instead of sending it as one Start/End Raster
   * Graphics block covering the whole page. A real (non-flat-test-shape)
   * high-resolution image can be tens of megabytes uncompressed once the
   * printer decodes it, and this device's onboard RAM cannot hold that -
   * confirmed by packet capture on 2026-08-01: a 131MB (600dpi A3 photo)
   * job arrived at the printer 100% complete and gap-free over TCP, yet
   * printed with its bottom portion missing, matching the Reference
   * Guide's documented behavior when an image overflows available RAM
   * ("the device immediately enters on-the-fly plotting mode ... prints
   * ... data it has received so far ... discards subsequent data").
   * Closing Raster Graphics (ESC*rC) after each band forces the device
   * to render and free that band before the next one arrives, so peak
   * memory use is bounded by the band size instead of the whole image.
   * End Raster Graphics leaves the CAP at the start of the next row (see
   * its spec), so consecutive Start Raster Graphics(at CAP) calls simply
   * continue the image downward with no gap or overlap between bands. */
  unsigned band_rows;
  if (band_target == 0)
  {
    band_rows = height; /* single band - no split, no ESC*rC mid-page */
  }
  else
  {
    band_rows = (unsigned)(band_target / bpl);
    if (band_rows < 1)
      band_rows = 1;
  }

  DBG("page=%d banding: bpl=%u band_rows=%u (~%.1fMB/band) bands=%u\n",
      page_num, bpl, band_rows, band_rows * (double)bpl / 1e6,
      (height + band_rows - 1) / band_rows);

  unsigned y = 0;
  while (y < height)
  {
    unsigned band_end = y + band_rows;
    if (band_end > height)
      band_end = height;

    fprintf(out, "\033*r1A");   /* Start raster graphics at CAP, unscaled. */

    /* Start Raster Graphics zeroes every seed row and resets the
     * compression method to 0, so both have to be re-established per
     * band. The error-diffusion state deliberately survives - see
     * dither_plane(). */
    for (int p = 0; p < (nplanes ? nplanes : 1); p++)
      memset(seed[p], 0, unit);
    int cur_method = -1;

    for (; y < band_end; y++)
    {
      ssize_t got = cupsRasterReadPixels(ras, line, bpl);
      if ((unsigned)got != bpl)
      {
        DBG("page=%d ABORT short-read at row=%u/%u got=%zd want=%u t=%.3f "
            "elapsed=%.3f total_out=%zu\n",
            page_num, y, height, got, bpl, now_seconds(),
            now_seconds() - t_start, total_out);
        FREE_PAGE_BUFFERS();
        return 0;
      }

      /* Apply gamma before separating, so every downstream stage - ink
       * amounts, dithering, seed rows, compressed bytes - describes the
       * same corrected image. */
      if (gamma_active)
        for (unsigned b = 0; b < bpl; b++)
          line[b] = gamma_lut[line[b]];

      if (nplanes)
      {
        separate_row(line, width, color_mode, ink);
        for (int p = 0; p < nplanes; p++)
        {
          if (dither_diffuse)
          {
            dither_plane(ink[p], width, err_cur[p], err_next[p], bits[p],
                         planebytes);
            /* Roll the diffusion window down one row. */
            int *tmp = err_cur[p];
            err_cur[p] = err_next[p];
            err_next[p] = tmp;
          }
          else
          {
            dither_plane_ordered(ink[p], width, y, bits[p], planebytes);
          }
        }
      }

      /* One transfer per plane; the last plane of the row uses W (which
       * advances the row), every earlier plane uses V (which advances
       * only the plane pointer). In chunky RGB mode there is a single
       * "plane" and it is simply the row itself. */
      for (int p = 0; p < (nplanes ? nplanes : 1); p++)
      {
        const unsigned char *src = nplanes ? bits[p] : line;

        /* Pick whichever method encodes this particular plane smallest.
         * Packbits wins on flat/blank areas (long byte runs), delta-row
         * wins where the plane barely changes from the row above, and
         * plain unencoded wins on dithered noise, where both compressors
         * would otherwise *expand* the data. Mixing is explicitly safe:
         * the seed row is updated by any row-based transfer, whatever
         * method produced it. */
        size_t n_pack = packbits_encode(src, unit, packed);
        size_t n_delta = delta_encode(src, seed[p], unit, delta);

        const unsigned char *enc = src;
        size_t n = unit;
        int method = 0;
        if (n_pack < n)
        {
          enc = packed; n = n_pack; method = 2;
        }
        if (n_delta < n)
        {
          enc = delta; n = n_delta; method = 3;
        }

        if (method != cur_method)
        {
          fprintf(out, "\033*b%dM", method);
          cur_method = method;
        }

        int last = (p == (nplanes ? nplanes : 1) - 1);
        fprintf(out, "\033*b%zu%c", n, last ? 'W' : 'V');
        size_t wrote = fwrite(enc, 1, n, out);
        total_out += wrote;
        memcpy(seed[p], src, unit); /* becomes this plane's next seed row */

        if (wrote != n)
        {
          DBG("page=%d ABORT short write at row=%u/%u plane=%d wrote=%zu "
              "want=%zu errno=%d (%s)\n",
              page_num, y, height, p, wrote, n, errno, strerror(errno));
          FREE_PAGE_BUFFERS();
          return 0;
        }
      }

      /* SIGPIPE is ignored (see main()), so a dead connection to the
       * printer shows up here as a short/failed write, not a signal -
       * if we don't check for it we'll happily "finish" the whole page
       * while silently sending nothing past the point the connection
       * died. */
      if (ferror(out))
      {
        DBG("page=%d ABORT write failed at row=%u/%u "
            "errno=%d (%s) t=%.3f elapsed=%.3f total_out=%zu\n",
            page_num, y, height, errno, strerror(errno),
            now_seconds(), now_seconds() - t_start, total_out);
        FREE_PAGE_BUFFERS();
        return 0;
      }
    }

    fprintf(out, "\033*rC"); /* End raster graphics: renders + frees the
                              * band, advances CAP to the next row. */

    if (ferror(out))
    {
      DBG("page=%d ABORT write failed ending band at row=%u/%u errno=%d "
          "(%s)\n",
          page_num, y, height, errno, strerror(errno));
      FREE_PAGE_BUFFERS();
      return 0;
    }

    DBG("page=%d band done: row=%u/%u t=%.3f elapsed=%.3f total_out=%zu\n",
        page_num, y, height, now_seconds(), now_seconds() - t_start,
        total_out);
  }

  FREE_PAGE_BUFFERS();
#undef FREE_PAGE_BUFFERS

  fprintf(out, "\033%%0B");  /* Back to HP-GL/2. */
  fprintf(out, "PG;");       /* Advance/print the page. */

  if (ferror(out))
  {
    DBG("page=%d ABORT write failed sending page trailer (PG;) errno=%d "
        "(%s)\n",
        page_num, errno, strerror(errno));
    return 0;
  }

  double t_end = now_seconds();
  DBG("page=%d emit_page done: rows=%u/%u t=%.3f elapsed=%.3f "
      "total_out=%zu\n",
      page_num, y, height, t_end, t_end - t_start, total_out);

  return 1;
}

int
main(int argc, char *argv[])
{
  if (argc < 6 || argc > 7)
  {
    fprintf(stderr,
            "Usage: rastertohpgl2rtl job-id user title copies options "
            "[file]\n");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  int fd = 0;
  if (argc == 7)
  {
    fd = open(argv[6], O_RDONLY);
    if (fd < 0)
    {
      fprintf(stderr, "ERROR: rastertohpgl2rtl: unable to open %s\n",
              argv[6]);
      return 1;
    }
  }

  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras)
  {
    fprintf(stderr, "ERROR: rastertohpgl2rtl: unable to open raster stream\n");
    return 1;
  }

  int direct = 1;
  out = connect_direct();
  if (!out)
  {
    direct = 0;
    out = stdout;
  }

  const char *title = argv[3];

  /* Gamma option comes through as e.g. "Gamma=g250" meaning 2.50 - the
   * PPD spells it that way because a bare "2.5" is not a legal PPD option
   * keyword. Anything unparseable falls back to 1.0 (no correction). */
  double gamma = 1.0;
  {
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    const char *g = cupsGetOption("Gamma", num_options, options);
    if (g)
    {
      if (*g == 'g' || *g == 'G')
        g++;
      double v = atof(g);
      if (v >= 10.0) /* "250" style: hundredths */
        v /= 100.0;
      if (v > 0.0)
        gamma = v;
    }

    /* Banding picks a preset; BandBytes overrides it with an exact figure
     * for tuning. Both are escape hatches - the default is no splitting. */
    const char *ra = cupsGetOption("RenderAlgorithm", num_options, options);
    if (ra && strcasecmp(ra, "Printer") != 0)
    {
      char *end;
      long v = strtol(ra, &end, 10);
      if (end != ra && v >= 0 && v <= 14)
        render_algorithm = (int)v;
    }

    const char *ql = cupsGetOption("QualityLevel", num_options, options);
    if (ql && strcasecmp(ql, "Printer") != 0)
    {
      if (!strcasecmp(ql, "Draft"))
        quality_level = 0;
      else if (!strcasecmp(ql, "Normal"))
        quality_level = 50;
      else if (!strcasecmp(ql, "Best"))
        quality_level = 100;
      else
      {
        /* A bare number is accepted too, for trying values against the
         * hardware - the device maps unsupported ones to a level it has. */
        char *end;
        long v = strtol(ql, &end, 10);
        if (end != ql && v >= 0 && v <= 100)
          quality_level = (int)v;
      }
    }

    const char *bnd = cupsGetOption("Banding", num_options, options);
    if (bnd)
    {
      if (!strcasecmp(bnd, "2MB"))
        band_target = 2 * 1024 * 1024;
      else if (!strcasecmp(bnd, "8MB"))
        band_target = 8 * 1024 * 1024;
      else
        band_target = 0; /* Off */
    }

    const char *bt = cupsGetOption("BandBytes", num_options, options);
    if (bt)
      band_target = (size_t)strtoull(bt, NULL, 10);

    const char *bg = cupsGetOption("BlackGeneration", num_options, options);
    if (bg)
    {
      if (!strcasecmp(bg, "Heavy"))
        black_start = 64;
      else if (!strcasecmp(bg, "Medium"))
        black_start = 128;
      else if (!strcasecmp(bg, "Light"))
        black_start = 192;
      else
        black_start = 0; /* Full */
    }

    const char *dm = cupsGetOption("Dither", num_options, options);
    if (dm && (!strcasecmp(dm, "Diffusion") || !strcasecmp(dm, "FS")))
      dither_diffuse = 1;

    const char *cm = cupsGetOption("ColorModel", num_options, options);
    if (cm)
    {
      if (!strcasecmp(cm, "RGB"))
        color_mode = MODE_RGB;
      else if (!strcasecmp(cm, "Gray") || !strcasecmp(cm, "KOnly"))
        color_mode = MODE_GRAY_K;
      else
        color_mode = MODE_KCMY;
    }
    cupsFreeOptions(num_options, options);
  }
  build_gamma_lut(gamma);

  DBG("=== job start: title=\"%s\" argv[5]=\"%s\" gamma=%.2f DEVICE_URI=\"%s\" "
      "direct=%d t=%.3f ===\n",
      title, argv[5], gamma,
      getenv("DEVICE_URI") ? getenv("DEVICE_URI") : "(null)",
      direct, now_seconds());

  cups_page_header2_t header;
  int page = 0;
  int ok = 1;
  int resolution = 300;

  while (cupsRasterReadHeader2(ras, &header))
  {
    /* No orientation handling here on purpose: the raster CUPS gives us
     * is already rotated into its final on-media layout, and PS/raster
     * geometry below is derived from that raster, so the whole pipeline
     * is orientation-agnostic. See emit_job_header() on why we always
     * tell the printer PORTRAIT ("don't rotate"). */
    DBG("page=%d cupsWidth=%u cupsHeight=%u cupsBytesPerLine=%u "
        "HWResolution=[%u,%u] PageSize=[%u,%u] Orientation=%u "
        "cupsColorSpace=%d cupsBitsPerColor=%u cupsCompression=%u "
        "title=\"%s\"\n",
        page + 1, header.cupsWidth, header.cupsHeight,
        header.cupsBytesPerLine, header.HWResolution[0],
        header.HWResolution[1], header.PageSize[0], header.PageSize[1],
        header.Orientation, header.cupsColorSpace, header.cupsBitsPerColor,
        header.cupsCompression, title);

    if (page == 0)
    {
      resolution = header.HWResolution[0] > 0 ? (int)header.HWResolution[0]
                                               : 300;
      /* PAPERWIDTH/PAPERLENGTH describe the *sheet*, so they come from
       * PageSize, not from the raster. The raster covers only the
       * imageable area, and declaring that as the media told the printer
       * the paper was smaller than it really is - it then applied its own
       * margins on top of our already-inset area and clipped the bottom.
       *
       * Taking these from PageSize is safe in landscape too: CUPS always
       * hands over a raster in the physical orientation of the media, and
       * PageSize follows the selected media the same way, including the
       * .Transverse sizes. PageSize is in points, PJL wants decipoints -
       * exactly 10x. */
      unsigned paper_w_dp = (unsigned)header.PageSize[0] * 10;
      unsigned paper_l_dp = (unsigned)header.PageSize[1] * 10;
      emit_job_header(title, resolution, paper_w_dp, paper_l_dp);
    }

    fprintf(stderr, "PAGE: %d 1\n", page + 1);

    if (!emit_page(ras, &header, page + 1))
    {
      ok = 0;
      break;
    }
    page++;
  }

  cupsRasterClose(ras);
  if (fd != 0)
    close(fd);

  if (page == 0)
  {
    fprintf(stderr, "ERROR: rastertohpgl2rtl: no pages found in job\n");
    DBG("=== job ABORT: no pages found t=%.3f ===\n", now_seconds());
    if (direct)
      fclose(out);
    return 1;
  }

  emit_job_trailer(title);
  fflush(out);
  if (ferror(out))
  {
    DBG("job trailer write FAILED errno=%d (%s)\n", errno, strerror(errno));
    ok = 0;
  }
  if (direct)
    fclose(out); /* Closes the socket too - signals EOF to the printer. */

  DBG("=== job end: pages=%d ok=%d direct=%d t=%.3f ===\n", page, ok, direct,
      now_seconds());

  return ok ? 0 : 1;
}
