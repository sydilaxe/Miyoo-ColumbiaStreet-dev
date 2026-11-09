# Columbia Street — jungle break sequencer for Miyoo Mini Plus / OnionOS
#
#   make          cross-compile dist/ColumbiaStreet/columbia (ARMv7 hard-float, glibc 2.28)
#                 and fetch the curated samples into dist/ColumbiaStreet/breaks and bass
#   make test     build the host harness and render screenshots + audio into build/test
#   make zip      package dist/ColumbiaStreet as build/ColumbiaStreet.zip for copying to SD:/App/
#   make install SD=/run/media/$USER/<card>   copy the app onto a mounted SD card
#   make package CROSS=<prefix>   build with the OnionOS gcc toolchain (no zig) and zip it as
#                 build/ColumbiaStreet-miyoo.zip, laid out to unzip at the root of the SD card (CI does this)
#   make update-test   install a fake release over a test app folder, the way the app updates itself
#   make version  print the version: CS_VERSION in src/engine.h, or VERSION=x.y.z when given
#                 (CI passes the release tag's version, so a release is built as v<tag>)

ZIG    ?= $(HOME)/.local/opt/zig-aarch64-linux-0.14.1/zig
TARGET := arm-linux-gnueabihf.2.28
SDL_INC := third_party/SDL-1.2.15-headers
SDL_LIB := third_party/onion-lib/libSDL-1.2.so.0

SRC    := src/engine.c src/breaks.c src/ui.c src/gfx.c src/sync.c src/aclock.c src/midi.c src/link.c
HDR    := src/engine.h src/breaks.h src/ui.h src/gfx.h src/sync.h src/aclock.h src/midi.h src/link.h src/font_data.h src/update.h
NET    := src/net.c src/update.c # the self-updater, on the Miyoo and in build/cs_update
VERSION := $(shell sed -n 's/^\#define CS_VERSION "\([^"]*\)".*/\1/p' src/engine.h)
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -fno-math-errno -DCS_VERSION='"$(VERSION)"'
APP    := dist/ColumbiaStreet
SAMPLES := $(APP)/breaks/AMEN_153bpm.wav

all: $(APP)/columbia $(APP)/icon.png $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt samples

# read by install.sh to tell whether the Miyoo is up to date; rewritten every time, since
# VERSION can come from the command line
$(APP)/VERSION: FORCE
	echo $(VERSION) > $@
FORCE:

samples: $(SAMPLES)
$(SAMPLES): tools/fetch_samples.sh
	sh tools/fetch_samples.sh $(APP)
	@touch $@

src/font_data.h: tools/mkfont.py
	python3 tools/mkfont.py /usr/share/fonts/google-noto/NotoSansMono-Bold.ttf $@

$(APP)/icon.png: tools/mkicon.py
	python3 tools/mkicon.py $@

# Onion's MI_AO/MI_GFX-backed SDL 1.2 (LGPL), linked against but not committed
SDL_URL := https://raw.githubusercontent.com/OnionUI/Onion/main/static/build/.tmp_update/lib/parasyte/libSDL-1.2.so.0
$(SDL_LIB):
	mkdir -p $(dir $@)
	curl -fsSL --retry 3 --retry-delay 2 -o $@ $(SDL_URL) || wget -qO $@ $(SDL_URL) || (rm -f $@; false)

# Mbed TLS (Apache-2.0) for the updater's HTTPS, from a pinned release, not committed; each
# toolchain builds its own libmbedtls.a from it with tools/mbedtls.mk
MBED_VER := 3.6.7
MBED_SHA := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBED_DIR := third_party/mbedtls
MBED_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBED_VER)/mbedtls-$(MBED_VER).tar.bz2
MBED_SRC := $(MBED_DIR)/.v$(MBED_VER)
$(MBED_SRC):
	rm -rf $(MBED_DIR) && mkdir -p $(MBED_DIR)
	curl -fsSL --retry 3 --retry-delay 2 -o $(MBED_DIR).tar.bz2 $(MBED_URL) || wget -qO $(MBED_DIR).tar.bz2 $(MBED_URL)
	echo "$(MBED_SHA)  $(MBED_DIR).tar.bz2" | sha256sum -c --quiet || (rm -f $(MBED_DIR).tar.bz2; false)
	tar -xjf $(MBED_DIR).tar.bz2 -C $(MBED_DIR) --strip-components=1
	rm $(MBED_DIR).tar.bz2
	touch $@

# the binary carries Mbed TLS, so the app folder carries its licence
$(APP)/LICENSE-mbedtls.txt: $(MBED_SRC)
	cp $(MBED_DIR)/LICENSE $@

build/zig/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/zig MBED_DIR=$(MBED_DIR) \
		CC="$(ZIG) cc -target $(TARGET) -mcpu=cortex_a7" AR="$(ZIG) ar" MBED_CFLAGS=-O2

build/host/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/host MBED_DIR=$(MBED_DIR) CC=cc AR=ar MBED_CFLAGS=-O2

$(APP)/columbia: $(SRC) $(NET) src/main_sdl.c $(HDR) src/net.h $(SDL_LIB) build/zig/libmbedtls.a
	$(ZIG) cc -target $(TARGET) -mcpu=cortex_a7 $(CFLAGS) -I$(SDL_INC) -I$(MBED_DIR)/include -o $@ \
		$(SRC) $(NET) src/main_sdl.c build/zig/libmbedtls.a $(SDL_LIB) -lm -lpthread -Wl,--allow-shlib-undefined -s

build/cs_test: $(SRC) src/main_test.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/main_test.c -lm -lpthread

test: build/cs_test $(SAMPLES)
	@mkdir -p build/test
	build/cs_test build/test $(APP)
	cd build/test && for f in *.ppm; do convert $$f $${f%.ppm}.png && rm $$f; done

build/cs_update: $(NET) src/main_update.c src/engine.h src/net.h src/update.h build/host/libmbedtls.a
	cc $(CFLAGS) -I$(MBED_DIR)/include -o $@ $(NET) src/main_update.c build/host/libmbedtls.a -lpthread

update-test: build/cs_update
	sh tools/update_test.sh

version:
	@echo $(VERSION)

zip: all
	@mkdir -p build
	rm -f build/ColumbiaStreet.zip
	cd dist && zip -r ../build/ColumbiaStreet.zip ColumbiaStreet -x "ColumbiaStreet/*.sav*" "ColumbiaStreet/log.txt"

install: all
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard" && exit 1)
	@test -d "$(SD)/App" || (echo "$(SD)/App not found — is this the OnionOS card?" && exit 1)
	mkdir -p "$(SD)/App/ColumbiaStreet"
	cp -r $(APP)/. "$(SD)/App/ColumbiaStreet/"
	sync

clean:
	rm -rf build $(APP)/columbia $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt

.PHONY: all samples test update-test version zip install clean FORCE

# the OnionOS toolchain's gcc instead of zig, e.g. in the aemiii91/miyoomini-toolchain container:
#   make package CROSS=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
CROSS ?= arm-linux-gnueabihf-
ARM := -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
build/gcc/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/gcc MBED_DIR=$(MBED_DIR) CC=$(CROSS)gcc AR=$(CROSS)ar \
		MBED_CFLAGS="-O2 $(ARM)"

miyoo: $(SRC) $(NET) src/main_sdl.c $(HDR) src/net.h $(SDL_LIB) build/gcc/libmbedtls.a
	$(CROSS)gcc $(ARM) $(CFLAGS) -I$(SDL_INC) -I$(MBED_DIR)/include -o $(APP)/columbia \
		$(SRC) $(NET) src/main_sdl.c build/gcc/libmbedtls.a $(SDL_LIB) -lm -lpthread -Wl,--allow-shlib-undefined -s

package: miyoo samples $(APP)/VERSION $(APP)/LICENSE-mbedtls.txt
	rm -rf build/sd build/ColumbiaStreet-miyoo.zip
	mkdir -p build/sd/App
	cp -r $(APP) build/sd/App/
	rm -f build/sd/App/ColumbiaStreet/*.sav* build/sd/App/ColumbiaStreet/log.txt
	chmod +x build/sd/App/ColumbiaStreet/launch.sh build/sd/App/ColumbiaStreet/columbia
	cd build/sd && zip -qr ../ColumbiaStreet-miyoo.zip App

.PHONY: miyoo package
