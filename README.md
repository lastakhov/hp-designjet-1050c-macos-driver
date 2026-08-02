# HP DesignJet 1050C driver for modern macOS

A CUPS driver for the HP DesignJet 1050C large-format plotter, which HP
stopped supporting at Mac OS X 10.7. It renders ordinary documents from any
macOS application to the plotter's native page description languages.

## Why this exists

The 1050C speaks neither PCL nor PostScript. Queried directly over SNMP, the
machine reports:

```
MANUFACTURER:Hewlett-Packard;
COMMAND SET:PML,MLC,PJL,HP-GL,HP-GL/2,RTL;
MODEL:DesignJet 1050C (C6074A);
```

So this driver converts CUPS raster into **HP RTL** raster wrapped in
**HP-GL/2**, wrapped in a **PJL** job, and ships it to port 9100.

## Installing

Download the `.pkg` from the latest CI run, or build it yourself:

```bash
make pkg
sudo installer -pkg build/HP-DesignJet-1050C-Driver.pkg -target /
```

The package is unsigned, so Gatekeeper will object to a double-click; use
the `installer` command above, or right-click → Open.

To build and install from source in one step:

```bash
./install.sh                 # driver only
./install.sh 192.168.1.100   # also create the print queues
```

Then add the printer through **System Settings → Printers & Scanners →
Add Printer**, choose an IP connection, and pick
*HP DesignJet 1050C, HP-GL/2+RTL* as the driver.

The filter is installed to `/Library/Printers/HPDesignJet1050C/` rather than
the usual `/usr/libexec/cups/filter/`, because the latter lives on the
SIP-sealed system volume and is not writable even as root on macOS 10.15+.
The PPD references it by absolute path.

## Two queues

| Queue | Use |
| --- | --- |
| `HP-DesignJet-1050C` | Any document. Rasterized by macOS, then converted to HP RTL. |
| `HP-DesignJet-1050C-Raw` | Pass-through for `.plt`/`.hpgl` files exported straight from CAD software — the plotter's native vector path, no rasterization. |

Apple's CUPS build removed `-m raw`, so the raw queue uses a minimal PPD
whose `cupsFilter2` filter field is `-` (no conversion) instead.

`examples/` holds a worked A3 architectural drawing — elevation, floor
plan, dimensions and title block — as native HP-GL/2, together with the
script that generates it. It is a quick way to check the vector path, and
it shows the `RO90`/`IP` picture header this plotter needs (see the axis
note under *Hardware quirks*). Confirmed printing correctly on the device:

```bash
lp -d HP-DesignJet-1050C-Raw examples/house_a3.plt
```

The whole drawing is 2 KB. The same page through the rasterising queue is
around 100 KB, and its lines are drawn rather than built out of dots.

## Options

- **Media size** — every size has a normal and a *Landscape (wide edge
  first)* variant. The landscape variants exist because on this plotter
  landscape means the sheet is physically fed wide-edge-first, not that the
  image should be rotated. Selecting one keeps the image unrotated. Sizes
  whose rotated width would exceed the 36 in carriage (A0, ANSI E, Arch E)
  have no landscape variant.
- **Resolution** — 300 or 600 dpi.
- **Color Mode** — `KCMY` (default) separates colour in the driver and sends
  four 1-bit planes, so black prints with black ink. `RGB` sends 24-bit
  colour and lets the printer decide how to lay ink down, which is what the
  driver used to do. `Gray` sends a single black plane. See *Colour
  separation* below for the trade-off.
- **Black Generation** — how much of a neutral is printed with black ink
  rather than mixed from CMY: `Full` (default, best for line work) through
  `Heavy`, `Medium` and `Light` (richer photographic shadows). Pure black
  stays pure black ink at every setting. `KCMY` only.
- **Halftone Method** — `Ordered` (default) or `Diffusion`. Applies to the
  separated modes; in `RGB` the printer does its own halftoning.
- **Raster Banding** — `Off` by default, meaning the page goes as a single
  raster block. Only raise this if a very large job loses its bottom edge;
  splitting costs visible seams (see *Hardware quirks*). `-o BandBytes=N`
  sets an exact size for tuning.

Which options actually do anything depends on the colour mode:

| | Black Generation | Halftone Method | Ink Density |
| --- | --- | --- | --- |
| `KCMY` | yes | yes | yes |
| `Gray` | no | yes | yes |
| `RGB` | no | no | yes |

`Gray` ignores black generation because it has a single black plane and no
chromatic channels to move a grey component out of. `RGB` ignores both
because separation and halftoning are left to the printer.
- **Ink Density (Gamma)** — driver-side gamma, 0.6 (most ink) to 3.0 (least).
  Applied as a 256-entry lookup table using the transfer function HP's
  reference guide specifies for this printer family. HP suggests ~2.5 for a
  DesignJet on HP Special Paper.

## Hardware quirks worth knowing

These cost real time to find, and the code comments point back here.

- **`PS` takes *length* then *width*, not width then height.** Per the
  reference guide, "the length always corresponds to the direction of the
  plot frame advance". Passing them the intuitive way round sets the
  hard-clip limits to a page as tall as the image is wide, silently cropping
  everything below — which looks exactly like a huge, mysterious bottom
  margin.
- **`ESC%1A` does not work on this unit; `ESC%0A` does.** The spec suggests
  entering RTL mode with parameter 1 ("use the current HP-GL/2 pen
  position"). On this firmware that silently fails to switch context, the
  raster is parsed as stray HP-GL/2, and a blank page is ejected. Parameter
  0 lands at the same (0,0) origin and works.
- **`@PJL SET MARGINS` has a `SMALLER` setting.** Asking for `NORMAL` costs
  usable page area on every job.
- **The device shifts rather than clips at the top margin.** Content placed
  above where the printer is willing to start does not get trimmed off the
  top — the whole image slides down instead, and whatever then hangs past
  the bottom is lost. So an under-declared *top* margin shows up as a
  *bottom* that is cut off, which is thoroughly misleading while debugging.
  Getting the top value right is what keeps the bottom on the sheet.
- **Measured margins beat the datasheet here.** HP publishes 17 mm leading
  and trailing; measured on cut sheet with `MARGINS=SMALLER` the real
  figures are 12 mm top and 15 mm bottom, with 5 mm sides. Using the
  published number threw away about 7 mm of usable height. These were
  obtained with a ruler print, and roll media may differ.
- **`PAPERWIDTH`/`PAPERLENGTH` describe the sheet, not the printable area.**
  Deriving them from the raster tells the printer the paper is smaller than
  it is; it then applies its own margins on top of the already-inset area
  and clips the bottom.
- **The X axis runs along the *longer* edge of the plot.** A portrait
  drawing sent as native HP-GL/2 therefore comes out turned 90° clockwise.
  `RO90` rotates the coordinate system back, and `IP` has to follow it —
  `P1`/`P2` keep their old coordinates through a rotation and would
  otherwise sit off the page. This only bites the raw queue; the
  rasterising queue is unaffected, because macOS lays the page out and the
  driver ships a finished bitmap. See `examples/`.
- **The stock CUPS socket backend is slow to this printer.** Its small,
  unbuffered writes hit the classic Nagle/delayed-ACK stall against the old
  JetDirect TCP stack. The filter therefore opens its own connection to
  `$DEVICE_URI` with `TCP_NODELAY` and a large buffer, falling back to
  stdout if that fails.
- **Splitting the raster into bands causes visible seams.** Closing each
  band with End Raster Graphics makes the device move the CAP and, per the
  spec, "fill the area through which the CAP moves with zeros" — which
  shows up as regular stripes across flat tone, spaced exactly one band
  apart (17.4 mm on A3 at 300 dpi). The page is now sent as a single
  block, which is also the form HP's own examples use. Banding had been
  added as a defence against the device running out of memory, on a
  misdiagnosis — the symptom it was meant to cure turned out to be the
  swapped `PS` parameters — and memory is in any case already handled by
  `ESC&a1N`, which lets the device print as data arrives instead of
  composing the whole page first.
- **Compression is chosen per row.** Packbits wins on flat and blank areas,
  delta-row wins on photographic areas where adjacent rows are nearly
  identical. Measured on a real 600 dpi photo job, delta-row gave 4.3x
  against Packbits' 1.5x; on line art the per-row choice is worth 30-40x
  overall. Mixing is safe because the seed row is updated by any row-based
  transfer.

## Colour separation

Left to itself this plotter composites black out of cyan, magenta and
yellow instead of using the black cartridge, so black text and line work
came out muddy, cyan-tinged and three times more expensive in ink. The
default `KCMY` mode fixes that by separating in the driver: convert to CMY,
pull the shared grey component into a real K channel, dither each channel
to one bit, and send four planes through the Simple Colour KCMY palette.
Confirmed on hardware — black now prints as black ink.

Two things about this are worth knowing before assuming it is a pure win.

**It does not make jobs smaller.** Four bits per pixel beats twenty-four
before compression, but dithering destroys the compressibility that contone
RGB enjoys, and four planes cost four transfer commands per row instead of
one:

| Page | RGB | KCMY (ordered) |
| --- | --- | --- |
| A3 line art | 74 KB | 111 KB |
| Full midtones | 42 KB | 158 KB |
| Mixed content | 92 KB | 83 KB |

Separated output is larger on flat art and smaller only on dense
photographs, where there was little to compress either way.

**It costs tonal resolution, though not spatial.** Average tone over one
8×8 dither cell is accurate to within 0.9/255, but only 65 distinct levels
survive per cell against 256 in contone, and a cell is 0.68 mm at 300 dpi
or 0.34 mm at 600 dpi. Content that is already pure black, white or fully
saturated — drawings, text, CAD, spot colour — reproduces exactly, so for
this plotter's main use there is no loss at all. Photographs lose tonal
resolution and may look better at 600 dpi, with `Dither=Diffusion`, or in
`ColorModel=RGB`.

Ordered dithering is the default over error diffusion on measurement, not
taste: its threshold pattern repeats every 8 pixels, which is exactly one
packed byte, so flat areas collapse under Packbits. Error diffusion turns
the same areas into incompressible noise — 1108 KB against 158 KB on a
full-page gradient.

How much of a neutral goes to black ink is adjustable with **Black
Generation**. `Full` sends every neutral to K, which is what line work
wants. Softer settings hold black back in the lighter tones and let CMY
carry them, which stops photographic shadows going flat.

The softer settings raise the tone at which black *starts* rather than
scaling black down. That distinction matters: simply multiplying K by some
factor below 1 would put coloured ink back underneath pure black text and
undo the thing this was written to fix. Raising the start point instead
leaves a fully saturated neutral mapping to K=255 at every setting, so
black stays pure black however far the setting is backed off — verified
end-to-end by decoding a rendered page and checking that none of its
23,448 solid-black bytes carry colour underneath, at any setting.

## Development

```bash
make        # build the universal (x86_64 + arm64) filter
make test   # round-trip tests for the raster encoders and gamma table
make pkg    # build the installer package
```

`make test` encodes known-awkward patterns with both compression methods and
decodes them with independently written decoders, so a misreading of the
spec cannot make a broken encoder look correct. It covers the multi-byte
offset escape and the 8-byte run limit in the delta format, which are the
easiest parts to get subtly wrong.

## Reference

HP-GL/2 and HP RTL Reference Guide, HP part number 5961-3526.
