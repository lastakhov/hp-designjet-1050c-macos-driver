CC       = clang
CFLAGS   = -Wall -Wextra -O2 -arch x86_64 -arch arm64
LIBS     = -lcups
FILTER   = filter/rastertohpgl2rtl
PPD_FILE     = ppd/HP-DesignJet_1050C.ppd
PPD_RAW_FILE = ppd/HP-DesignJet_1050C_Raw.ppd

# Install locations. /usr/libexec/cups/filter is on the SIP-sealed system
# volume on macOS 10.15+ and is NOT writable, even as root - so the filter
# is installed under /Library instead, and the PPD's cupsFilter2 line
# references it by absolute path.
FILTER_DIR = /Library/Printers/HPDesignJet1050C
PPD_DIR    = /Library/Printers/PPDs/Contents/Resources
QUEUE      = HP-DesignJet-1050C
RAW_QUEUE  = HP-DesignJet-1050C-Raw
# Override for your own plotter, e.g. make queue PRINTER_IP=192.168.2.103
PRINTER_IP ?= 192.168.1.100

PKG_ID      = com.local.hpdesignjet1050c
PKG_VERSION = 1.3.0
PKG_ROOT    = build/pkgroot
PKG_OUT     = build/HP-DesignJet-1050C-Driver.pkg

.PHONY: all clean install uninstall queue raw-queue pkg test

all: $(FILTER)

$(FILTER): filter/rastertohpgl2rtl.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# Round-trip tests for the raster encoders and gamma table. Built for the
# host arch only - it is a test binary, not something we ship.
test: filter/selftest.c filter/rastertohpgl2rtl.c
	mkdir -p build
	$(CC) -Wall -Wextra -O2 -o build/selftest filter/selftest.c $(LIBS)
	./build/selftest

clean:
	rm -f $(FILTER)
	rm -rf build

install: $(FILTER)
	sudo mkdir -p $(FILTER_DIR)
	sudo install -m 755 -o root -g wheel $(FILTER) $(FILTER_DIR)/rastertohpgl2rtl
	sudo mkdir -p $(PPD_DIR)
	sudo install -m 644 -o root -g wheel $(PPD_FILE) $(PPD_DIR)/HP-DesignJet_1050C.ppd
	sudo install -m 644 -o root -g wheel $(PPD_RAW_FILE) $(PPD_DIR)/HP-DesignJet_1050C_Raw.ppd
	sudo install -m 644 -o root -g wheel LICENSE $(FILTER_DIR)/LICENSE
	sudo launchctl kickstart -k system/org.cups.cupsd || true

uninstall:
	sudo lpadmin -x $(QUEUE) || true
	sudo lpadmin -x $(RAW_QUEUE) || true
	sudo rm -rf $(FILTER_DIR)
	sudo rm -f $(PPD_DIR)/HP-DesignJet_1050C.ppd
	sudo rm -f $(PPD_DIR)/HP-DesignJet_1050C_Raw.ppd

queue: install
	sudo lpadmin -p $(QUEUE) -E -v socket://$(PRINTER_IP):9100 \
		-P $(PPD_DIR)/HP-DesignJet_1050C.ppd -o printer-is-shared=false

# Filterless pass-through queue: for HP-GL/2 (.plt/.hpgl) files exported
# directly by CAD software. Apple's CUPS build dropped "-m raw" support,
# so this uses a minimal PPD whose *cupsFilter2 filter field is "-"
# (no conversion) instead - CUPS ships the document to the printer
# exactly as given, which is what the plotter's native vector language
# wants.
raw-queue: install
	sudo lpadmin -p $(RAW_QUEUE) -E -v socket://$(PRINTER_IP):9100 \
		-P $(PPD_DIR)/HP-DesignJet_1050C_Raw.ppd -o printer-is-shared=false

# Build a standalone, double-clickable .pkg installer. Unsigned (no Apple
# Developer ID here), so Gatekeeper will require "right-click > Open" or
# `sudo installer -pkg ... -target /` on the machine that installs it.
pkg: $(FILTER)
	rm -rf $(PKG_ROOT)
	mkdir -p $(PKG_ROOT)$(FILTER_DIR)
	mkdir -p $(PKG_ROOT)$(PPD_DIR)
	install -m 755 $(FILTER) $(PKG_ROOT)$(FILTER_DIR)/rastertohpgl2rtl
	install -m 644 $(PPD_FILE) $(PKG_ROOT)$(PPD_DIR)/HP-DesignJet_1050C.ppd
	install -m 644 $(PPD_RAW_FILE) $(PKG_ROOT)$(PPD_DIR)/HP-DesignJet_1050C_Raw.ppd
	install -m 644 LICENSE $(PKG_ROOT)$(FILTER_DIR)/LICENSE
	mkdir -p build
	pkgbuild \
		--root $(PKG_ROOT) \
		--identifier $(PKG_ID) \
		--version $(PKG_VERSION) \
		--install-location / \
		--scripts scripts \
		$(PKG_OUT)
	@echo "Built $(PKG_OUT)"
