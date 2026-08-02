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
./install.sh 192.168.2.103   # also create the print queues
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

## Options

- **Media size** — every size has a normal and a *Landscape (wide edge
  first)* variant. The landscape variants exist because on this plotter
  landscape means the sheet is physically fed wide-edge-first, not that the
  image should be rotated. Selecting one keeps the image unrotated. Sizes
  whose rotated width would exceed the 36 in carriage (A0, ANSI E, Arch E)
  have no landscape variant.
- **Resolution** — 300 or 600 dpi.
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
- **The stock CUPS socket backend is slow to this printer.** Its small,
  unbuffered writes hit the classic Nagle/delayed-ACK stall against the old
  JetDirect TCP stack. The filter therefore opens its own connection to
  `$DEVICE_URI` with `TCP_NODELAY` and a large buffer, falling back to
  stdout if that fails.
- **Raster is sent in bands.** Each band is closed with End Raster Graphics
  so the device renders and frees it, bounding peak memory instead of
  requiring the whole decompressed image to fit at once.
- **Compression is chosen per row.** Packbits wins on flat and blank areas,
  delta-row wins on photographic areas where adjacent rows are nearly
  identical. Measured on a real 600 dpi photo job, delta-row gave 4.3x
  against Packbits' 1.5x; on line art the per-row choice is worth 30-40x
  overall. Mixing is safe because the seed row is updated by any row-based
  transfer.

## Known limitation

Black is composited by the printer from CMY inks rather than drawn from the
black cartridge, because the driver sends 24-bit RGB and lets the device
decide how to lay ink down. Fixing that means separating to KCMY and
dithering on the Mac, which would also cut data volume by roughly another
6x. Not done yet.

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
