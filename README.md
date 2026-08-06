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

Grab the `.pkg` from the
[latest release](https://github.com/lastakhov/hp-designjet-1050c-macos-driver/releases/latest)
— no GitHub account needed — and install it:

```bash
sudo installer -pkg HP-DesignJet-1050C-Driver.pkg -target /
```

The package is unsigned, so Gatekeeper will object to a double-click; use
the `installer` command above, or right-click → Open. It is a universal
binary and runs on both Intel and Apple Silicon.

Or build it yourself:

```bash
make pkg
sudo installer -pkg build/HP-DesignJet-1050C-Driver.pkg -target /
```

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
- **Resolution** — 600 dpi by default, matching the device's native grid
  and what HP's driver sends. 300 dpi is available and roughly quarters the
  data, which is worth having on very large sheets.
- **Print Quality** — `QL` in the HP-GL/2 picture header: `Draft`, `Normal`,
  `Best`, or `Printer` (default) to leave the front panel in charge. Draft
  is visibly coarser on this device, so the instruction is definitely
  honoured.
- **Color Mode** — `RGB` (default) sends contone colour and lets the device
  screen it, which gives both clean black and clean flat tone. `KCMY`
  separates in the driver and sends four 1-bit planes for explicit control
  over ink placement, at the cost of banding on flat areas. `Gray` sends a
  single black plane. See *Colour and halftoning* below.
- **Printer Halftone** — which screen the device applies to contone data,
  `13` (scatter dither) by default, matching HP's own driver. Leaving this
  unset is what made black look muddy in earlier versions. `RGB` only.
- **Black Generation** — how much of a neutral is printed with black ink
  rather than mixed from CMY: `Full` (default, best for line work) through
  `Heavy`, `Medium` and `Light` (richer photographic shadows). Pure black
  stays pure black ink at every setting. `KCMY` only.
- **Halftone Method** — the driver's own dither, `Ordered` (default) or
  `Diffusion`. `KCMY` and `Gray` only; in `RGB` the device screens instead.
- **Ink Density (Gamma)** — driver-side gamma, 0.6 (most ink) to 3.0 (least).
  Applied as a 256-entry lookup table using the transfer function HP's
  reference guide specifies for this printer family. HP suggests ~2.5 for a
  DesignJet on HP Special Paper.
- **Let Printer Scale Raster** — `On` by default. Declares the image size
  and the area it should fill and lets the device resample, instead of
  placing pixels one for one. This is what keeps smooth gradients from
  banding; see *Hardware quirks*. `RGB` only.
- **Print While Receiving** — `Off` by default, letting the device compose
  the page before printing rather than committing each swath as it
  arrives. Turn on only if a very large job exhausts it.
- **Raster Banding** — `Off` by default, meaning the page goes as a single
  raster block. Only raise this if a very large job loses its bottom edge;
  splitting costs visible seams (see *Hardware quirks*). `-o BandBytes=N`
  sets an exact size for tuning.

Which options actually do anything depends on the colour mode:

| | Black Generation | Halftone Method | Printer Halftone | Ink Density |
| --- | --- | --- | --- | --- |
| `RGB` | no | no | **yes** | yes |
| `KCMY` | yes | yes | no | yes |
| `Gray` | no | yes | no | yes |

`Gray` ignores black generation because it has a single black plane and no
chromatic channels to move a grey component out of. `RGB` ignores both the
driver's separation and its dither, because it hands that work to the
device — and correspondingly it is the only mode where Printer Halftone
does anything. Print Quality and Media Size apply everywhere.

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
- **Print quality lives in HP-GL/2, not PJL.** This device answers PJL
  `INFO ID` and `INFO CONFIG` but returns `"?"` for `INFO VARIABLES` and
  ignores `DINQUIRE` for every quality-related variable, which makes it look
  as though quality cannot be driven from the host at all. It can — through
  the HP-GL/2 `QL` instruction in the picture header.
- **Naming the halftone matters.** Sending contone RGB without `ESC*t#J`
  leaves the device on a default screen that renders black muddy and
  cyan-tinged. Asking for scatter dither explicitly, as HP's driver does,
  fixes it.
- **A host-side dither bands; the device's does not.** The head lays down a
  swath at a time and a driver cannot know where those swaths fall, so its
  dither pattern does not line up across them. Anything screened in the
  driver shows horizontal banding on flat tone that screening in the device
  does not.
- **Placing raster pixels one for one bands on gradients.** With
  `ESC*r1A` the device takes the pixels exactly as given and has no freedom
  in how they land relative to the swaths it prints, which shows as regular
  horizontal banding across smooth tone. Declaring the image size and the
  area to fill instead, and starting with `ESC*r3A` so the device
  resamples, lets the firmware line its own dot rows up with those swaths.
  The same photograph bands one way and is clean the other; HP's driver
  always scales. Flat fills happen not to expose this, which is why it
  survived several rounds of banding work that only tested flat tone.
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

## Colour and halftoning

Left to itself this plotter composites black out of cyan, magenta and
yellow instead of using the black cartridge, so black text came out muddy
and cyan-tinged. The fix turned out not to be where it first appeared.

The driver now sends contone RGB and asks the device to screen it, naming
the halftone explicitly with `ESC*t#J` (scatter dither, which is what HP's
own Windows driver selects). That gives clean black **and** clean flat
tone. Leaving the halftone unspecified — which this driver did originally —
is what produced the muddy black that started the whole investigation.

The alternative, `ColorModel=KCMY`, separates in the driver instead:
convert to CMY, pull the shared grey component into a real K channel,
dither each channel to one bit, and send four planes through the Simple
Colour KCMY palette. It also produces clean black, and it is kept because
it gives explicit control over exactly which ink lands where. But it is no
longer the default, for one decisive reason.

**Dithering in the driver bands on flat tone.** The device prints a swath
at a time, and a host-side dither cannot know where those swaths fall, so
its pattern does not line up across them. Printing the same flat greys
both ways makes it obvious: separated output shows horizontal banding,
printer-screened output does not. HP's driver never hits this because it
does not rasterize flat fills at all — a captured job shows zero raster
commands and pure HP-GL/2 polygons, leaving every halftoning decision to
the firmware.

That last point also bounds what this driver can do. By the time CUPS
hands over a page the vectors are gone and only a bitmap remains, so the
fully vector path is not reachable from a raster filter — it would need a
different filter chain that takes PDF directly.

`KCMY` is still worth choosing when ink placement matters more than smooth
tone, and it costs nothing on pure black-and-white artwork, where every
pixel is 0 or 255 and dithering does nothing. Note it also does not make
jobs smaller — four bits per pixel beats twenty-four before compression,
but dithering destroys the compressibility contone RGB enjoys:

| Page | RGB | KCMY |
| --- | --- | --- |
| A3 line art | 74 KB | 111 KB |
| Full midtones | 42 KB | 158 KB |
| Mixed content | 92 KB | 83 KB |

Within `KCMY`, ordered dithering is the default over error diffusion on
measured size, not taste: its threshold pattern repeats every 8 pixels,
exactly one packed byte, so flat areas collapse under Packbits, while
error diffusion turns them into incompressible noise — 1108 KB against
158 KB on a full-page gradient. Black generation defaults to full grey
component replacement; backing it off raises the tone at which black
starts rather than scaling black down, so pure black stays pure black ink
at every setting.

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

## Licence

GPL-2.0-or-later — the usual choice for CUPS filters, and the same terms
Gutenprint, HPLIP and foomatic use. See [LICENSE](LICENSE).
