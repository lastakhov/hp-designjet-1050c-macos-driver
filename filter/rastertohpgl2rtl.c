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
 *   BP5,1;IN;PS w,h;TR0;        Begin plot (no autorotate), init, plot size
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
 *   ESC * v 6 W [0,3,3,8,8,8]   Configure Image Data: direct-by-pixel RGB888
 *   { ESC * r 1 A                 Start raster graphics at CAP (unscaled)
 *     { ESC * b # M               Compression method, emitted only when it
 *                                 changes: 2 (Packbits) or 3 (delta-row),
 *                                 chosen per row by whichever is smaller
 *       ESC * b # W <data> }*     One combined command per row of this band
 *     ESC * r C                   End raster graphics: renders/frees the
 *                                 band, advances CAP to the next row }*
 *                               Repeated per band - see the note by
 *                               emit_page() on why the image is banded
 *                               instead of one Start/End Raster Graphics
 *                               pair around the whole page
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

/* Target uncompressed bytes per raster band - see the long comment in
 * emit_page() for why banding exists at all. 2MB/band is a conservative
 * guess for a 25-year-old printer's available RAM; it's a starting point
 * pending real calibration against the hardware, not a measured limit. */
#define BAND_TARGET_BYTES (2 * 1024 * 1024)

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
  fprintf(out, "BP5,1;IN;PS%ld,%ld;TR0;", plot_h, plot_w);
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

  /* Configure Image Data: color space=0, pixel encoding=3 (direct by
   * pixel), bits/index=3 (unused in direct mode), bits/R=G=B=8. */
  fprintf(out, "\033*v6W");
  {
    unsigned char cfg[6] = {0, 3, 3, 8, 8, 8};
    fwrite(cfg, 1, sizeof(cfg), out);
  }

  unsigned char *line = malloc(bpl);
  unsigned char *packed = malloc(bpl + (bpl / 128) + 16);
  unsigned char *delta = malloc(2 * (size_t)bpl + 64);
  unsigned char *seed = calloc(1, bpl); /* previous row, for method 3 */
  if (!line || !packed || !delta || !seed)
  {
    DBG("page=%d ABORT out of memory (bpl=%u)\n", page_num, bpl);
    free(line);
    free(packed);
    free(delta);
    free(seed);
    return 0;
  }

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
  unsigned band_rows = (unsigned)(BAND_TARGET_BYTES / bpl);
  if (band_rows < 1)
    band_rows = 1;

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

    /* Start Raster Graphics zeroes the seed row and resets the compression
     * method to 0, so both have to be re-established per band. */
    memset(seed, 0, bpl);
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
        free(line);
        free(packed);
        free(delta);
        free(seed);
        return 0;
      }

      /* Apply gamma before compressing, so the seed row and the
       * compressed bytes all describe the same corrected image. */
      if (gamma_active)
        for (unsigned b = 0; b < bpl; b++)
          line[b] = gamma_lut[line[b]];

      /* Pick whichever row-based method encodes this particular row
       * smallest. Packbits wins on flat/blank areas (long byte runs),
       * delta-row wins on photographic areas (rows nearly identical to
       * the one above); measured on a real job, choosing per row beats
       * either method alone. Mixing is explicitly safe: the seed row is
       * updated by *any* row-based transfer, not just method-3 ones. */
      size_t n_pack = packbits_encode(line, bpl, packed);
      size_t n_delta = delta_encode(line, seed, bpl, delta);

      const unsigned char *enc;
      size_t n;
      int method;
      if (n_delta < n_pack)
      {
        enc = delta; n = n_delta; method = 3;
      }
      else
      {
        enc = packed; n = n_pack; method = 2;
      }

      if (method != cur_method)
      {
        fprintf(out, "\033*b%dM", method);
        cur_method = method;
      }

      fprintf(out, "\033*b%zuW", n);
      size_t wrote = fwrite(enc, 1, n, out);
      total_out += wrote;
      memcpy(seed, line, bpl); /* this row becomes the next seed row */

      /* SIGPIPE is ignored (see main()), so a dead connection to the
       * printer shows up here as a short/failed write, not a signal -
       * if we don't check for it we'll happily "finish" the whole page
       * while silently sending nothing past the point the connection
       * died. */
      if (wrote != n || ferror(out))
      {
        DBG("page=%d ABORT write failed at row=%u/%u wrote=%zu want=%zu "
            "errno=%d (%s) t=%.3f elapsed=%.3f total_out=%zu\n",
            page_num, y, height, wrote, n, errno, strerror(errno),
            now_seconds(), now_seconds() - t_start, total_out);
        free(line);
        free(packed);
        free(delta);
        free(seed);
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
      free(line);
      free(packed);
      free(delta);
      free(seed);
      return 0;
    }

    DBG("page=%d band done: row=%u/%u t=%.3f elapsed=%.3f total_out=%zu\n",
        page_num, y, height, now_seconds(), now_seconds() - t_start,
        total_out);
  }

  free(line);
  free(packed);
  free(delta);
  free(seed);

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
      /* Derive the declared media size from the raster itself rather than
       * from PageSize[]. PageSize is the *unrotated* page in points, so on
       * a landscape job it stays portrait-shaped while the raster we are
       * about to send is landscape-shaped - declaring that mismatched,
       * too-narrow media to the printer invites exactly the kind of
       * silent edge clipping that the PS length/width mix-up used to
       * cause. The raster dimensions are the one description of the page
       * that is always already in final on-media orientation, so PS,
       * PAPERWIDTH and PAPERLENGTH are all derived from it and can never
       * disagree with each other. 720 decipoints per inch. */
      double hx = header.HWResolution[0] > 0 ? header.HWResolution[0] : 300;
      double hy = header.HWResolution[1] > 0 ? header.HWResolution[1] : 300;
      unsigned paper_w_dp = (unsigned)lround(header.cupsWidth / hx * 720.0);
      unsigned paper_l_dp = (unsigned)lround(header.cupsHeight / hy * 720.0);
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
