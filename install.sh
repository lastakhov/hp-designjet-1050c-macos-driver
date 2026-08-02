#!/bin/bash
# Copyright (C) 2026 Leonid Astakhov
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version. See the LICENSE file for details.

# Builds and installs the HP DesignJet 1050C CUPS driver (PPD + HP-GL/2/RTL
# filter) from source. Alternative to the prebuilt .pkg (`make pkg`) for
# anyone who'd rather build locally. Requires Xcode Command Line Tools
# (for clang and the CUPS headers) - install with `xcode-select --install`
# if `clang` is not found.
#
# Usage:
#   ./install.sh                 # just install the driver files
#   ./install.sh 192.168.1.100   # also register both print queues at that IP
#
# Two queues are registered when an IP is given:
#   HP-DesignJet-1050C       rasterizes any document (PDF/Preview/Word/...)
#                             through our HP-GL/2+RTL filter - general use.
#   HP-DesignJet-1050C-Raw    filterless pass-through, for HP-GL/2 (.plt/
#                             .hpgl) files exported directly by CAD
#                             software - the plotter's native vector path,
#                             no rasterization.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FILTER_SRC="$SCRIPT_DIR/filter/rastertohpgl2rtl.c"
PPD_SRC="$SCRIPT_DIR/ppd/HP-DesignJet_1050C.ppd"
PPD_RAW_SRC="$SCRIPT_DIR/ppd/HP-DesignJet_1050C_Raw.ppd"

# /usr/libexec/cups/filter is on the SIP-sealed system volume on macOS
# 10.15+ and cannot be written to, even as root - so the filter lives
# under /Library instead, and the PPD points at it by absolute path.
FILTER_DIR="/Library/Printers/HPDesignJet1050C"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
QUEUE_NAME="HP-DesignJet-1050C"
RAW_QUEUE_NAME="HP-DesignJet-1050C-Raw"
PRINTER_IP="${1:-}"

if ! command -v clang >/dev/null 2>&1; then
  echo "error: clang not found. Install Xcode Command Line Tools first:" >&2
  echo "  xcode-select --install" >&2
  exit 1
fi

if [[ ! -f "$FILTER_SRC" || ! -f "$PPD_SRC" || ! -f "$PPD_RAW_SRC" ]]; then
  echo "error: run this script from inside the project directory" >&2
  exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "==> Building universal filter binary (x86_64 + arm64)..."
clang -Wall -Wextra -O2 -arch x86_64 -arch arm64 \
  -o "$WORKDIR/rastertohpgl2rtl" "$FILTER_SRC" -lcups

echo "==> Installing filter to $FILTER_DIR (sudo required)..."
sudo mkdir -p "$FILTER_DIR"
sudo install -m 755 -o root -g wheel "$WORKDIR/rastertohpgl2rtl" \
  "$FILTER_DIR/rastertohpgl2rtl"

echo "==> Installing PPDs to $PPD_DIR (sudo required)..."
sudo mkdir -p "$PPD_DIR"
sudo install -m 644 -o root -g wheel "$PPD_SRC" \
  "$PPD_DIR/HP-DesignJet_1050C.ppd"
sudo install -m 644 -o root -g wheel "$PPD_RAW_SRC" \
  "$PPD_DIR/HP-DesignJet_1050C_Raw.ppd"

echo "==> Restarting cupsd so it picks up the new PPD..."
sudo launchctl kickstart -k system/org.cups.cupsd || true

if [[ -n "$PRINTER_IP" ]]; then
  echo "==> Registering print queue '$QUEUE_NAME' at $PRINTER_IP:9100..."
  sudo lpadmin -p "$QUEUE_NAME" -E -v "socket://$PRINTER_IP:9100" \
    -P "$PPD_DIR/HP-DesignJet_1050C.ppd" -o printer-is-shared=false

  echo "==> Registering raw pass-through queue '$RAW_QUEUE_NAME' at $PRINTER_IP:9100..."
  sudo lpadmin -p "$RAW_QUEUE_NAME" -E -v "socket://$PRINTER_IP:9100" \
    -P "$PPD_DIR/HP-DesignJet_1050C_Raw.ppd" -o printer-is-shared=false

  echo "==> Done. Queues '$QUEUE_NAME' (rasterized) and '$RAW_QUEUE_NAME'"
  echo "    (raw HP-GL/2 pass-through) are ready."
else
  echo "==> Done. Driver installed."
  echo "    Add the printer via System Settings > Printers & Scanners >"
  echo "    Add Printer, choose an IP connection, and select"
  echo "    \"HP DesignJet 1050C HP-GL2+RTL\" as the driver -"
  echo "    or re-run this script with the printer's IP as an argument."
fi
