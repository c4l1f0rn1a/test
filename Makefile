VERSION   ?= 0.1.0
PREFIX    ?= /usr/local
DESTDIR   ?=

# PJSIP built by `make pjsip` (static, into third_party/pj-install).
PJ_PREFIX ?= $(CURDIR)/third_party/pj-install
PKG_CONFIG = PKG_CONFIG_PATH="$(PJ_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config

CC       = gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -DSIPPHONE_VERSION=\"$(VERSION)\"
PKGS     = gtk+-3.0

# Tray icon (StatusNotifierItem, works in KDE Plasma) is optional.
ifeq ($(shell $(PKG_CONFIG) --exists ayatana-appindicator3-0.1 && echo yes),yes)
PKGS    += ayatana-appindicator3-0.1
CFLAGS  += -DHAVE_APPINDICATOR
endif

CFLAGS  += $(shell $(PKG_CONFIG) --cflags $(PKGS) libpjproject)
LDLIBS  += $(shell $(PKG_CONFIG) --libs $(PKGS)) \
           $(shell $(PKG_CONFIG) --static --libs libpjproject)

SRC = src/main.c src/ui.c src/engine.c src/config.c
OBJ = $(SRC:.c=.o)
BIN = sipphone

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- PJSIP (static, minimal) ----
PJ_VERSION ?= 2.17
PJ_SRC      = third_party/pjproject

$(PJ_SRC):
	git clone --depth 1 --branch $(PJ_VERSION) \
		https://github.com/pjsip/pjproject.git $(PJ_SRC)

pjsip: $(PJ_SRC)
	cd $(PJ_SRC) && CFLAGS="-O2 -fPIC" ./configure --prefix=$(PJ_PREFIX) \
		--disable-shared --disable-pjsua2 --disable-upnp \
		--disable-video --disable-sdl --disable-ffmpeg --disable-v4l2 \
		--disable-openh264 --disable-libyuv --disable-vpx \
		--disable-g7221-codec --disable-ilbc-codec --disable-gsm-codec \
		--disable-l16-codec --disable-bcg729 --disable-silk --disable-lyra
	$(MAKE) -C $(PJ_SRC) dep
	$(MAKE) -C $(PJ_SRC)
	$(MAKE) -C $(PJ_SRC) install

# ---- install ----
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 data/sipphone.desktop \
		$(DESTDIR)$(PREFIX)/share/applications/io.github.c4l1f0rn1a.SipPhone.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) \
		$(DESTDIR)$(PREFIX)/share/applications/io.github.c4l1f0rn1a.SipPhone.desktop

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all pjsip install uninstall clean
