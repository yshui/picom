# Use tab to indent recipe lines, spaces to indent other lines, otherwise
# GNU make may get unhappy.

CC ?= gcc

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

PACKAGES = x11 xcomposite xfixes xdamage xrender xext xrandr
LIBS = -lm -lrt
INCS =

# === Configuration flags ===
CFG =

# ==== libevent ====
# Seemingly, libevent1 has no pkg-config file!
# FreeBSD 9.1 probably has issues handling --atleast-version in pkg-config.
ifeq "$(shell pkg-config --modversion --print-errors libevent)" ""
  $(warning libevent-2.0 not found, assuming libevent-1.4.x.)
  CFG += -DCONFIG_LIBEVENT_LEGACY
  LIBS += -levent
else
  # Using pkg-config for linking with libevent will result in linking with
  # libevent.so instead of the smaller libevent_core.so. But FreeBSD keeps
  # libevent2 .so files at a separate place, and we must learn it from
  # pkg-config.
  LIBS += $(shell pkg-config --libs libevent)
  INCS += $(shell pkg-config --cflags libevent)
endif

# ==== libconfig ====
ifeq "$(NO_LIBCONFIG)" ""
  CFG += -DCONFIG_LIBCONFIG
  PACKAGES += libconfig

  # libconfig-1.3* does not define LIBCONFIG_VER* macros, so we use
  # pkg-config to determine its version here
  CFG += $(shell pkg-config --atleast-version=1.4 libconfig || echo '-DCONFIG_LIBCONFIG_LEGACY')
endif

# ==== PCRE regular expression ====
ifeq "$(NO_REGEX_PCRE)" ""
  CFG += -DCONFIG_REGEX_PCRE
  LIBS += $(shell pcre-config --libs)
  INCS += $(shell pcre-config --cflags)
  ifeq "$(NO_REGEX_PCRE_JIT)" ""
    CFG += -DCONFIG_REGEX_PCRE_JIT
  endif
endif

# ==== DRM VSync ====
ifeq "$(NO_VSYNC_DRM)" ""
  CFG += -DCONFIG_VSYNC_DRM
endif

# ==== OpenGL VSync ====
ifeq "$(NO_VSYNC_OPENGL)" ""
  CFG += -DCONFIG_VSYNC_OPENGL
  LIBS += -lGL
endif

# ==== D-Bus ====
# ifeq "$(NO_DBUS)" ""
#   CFG += -DCONFIG_DBUS
#   PACKAGES += dbus-1
# endif

# === Version string ===
COMPTON_VERSION ?= git-$(shell git describe --always)
CFG += -DCOMPTON_VERSION="\"$(COMPTON_VERSION)\""

LDFLAGS ?= -Wl,-O1 -Wl,--as-needed
CFLAGS ?= -DNDEBUG -O2 -D_FORTIFY_SOURCE=2 $(LDFLAGS)
CFLAGS += $(CFG)

LIBS += $(shell pkg-config --libs $(PACKAGES))
INCS += $(shell pkg-config --cflags $(PACKAGES))

CFLAGS += -Wall -std=c99
OBJS = compton.o

# === Recipes ===
.DEFAULT_GOAL := compton

%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) $(INCS) -c src/$*.c

compton: $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

docs:
	# HTML documentation
	asciidoc man/compton.1.asciidoc
	asciidoc man/compton-trans.1.asciidoc
	# man page
	a2x --format manpage man/compton.1.asciidoc
	a2x --format manpage man/compton-trans.1.asciidoc

install: compton
	@install -Dm755 compton "$(DESTDIR)$(BINDIR)"/compton
	@install -Dm755 bin/compton-trans "$(DESTDIR)$(BINDIR)"/compton-trans
	@install -Dm644 man/compton.1 "$(DESTDIR)$(MANDIR)"/compton.1
	@install -Dm644 man/compton-trans.1 "$(DESTDIR)$(MANDIR)"/compton-trans.1

uninstall:
	@rm -f "$(DESTDIR)$(BINDIR)/compton"
	@rm -f "$(DESTDIR)$(BINDIR)/compton-trans"
	@rm -f "$(DESTDIR)$(MANDIR)/compton.1"
	@rm -f "$(DESTDIR)$(MANDIR)/compton-trans.1"

clean:
	@rm -f $(OBJS) compton

.PHONY: uninstall clean docs
