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
  LIBS := -lGL $(LIBS)
endif

# ==== D-Bus ====
# ifeq "$(NO_DBUS)" ""
#   CFG += -DCONFIG_DBUS
#   PACKAGES += dbus-1
# endif

# === Version string ===
COMPTON_VERSION ?= git-$(shell git describe --always --dirty)-$(shell git log -1 --date=short --pretty=format:%cd)
CFG += -DCOMPTON_VERSION="\"$(COMPTON_VERSION)\""

LDFLAGS ?= -Wl,-O1 -Wl,--as-needed
CFLAGS ?= -DNDEBUG -O2 -D_FORTIFY_SOURCE=2
CFLAGS += $(CFG)

LIBS += $(shell pkg-config --libs $(PACKAGES))
INCS += $(shell pkg-config --cflags $(PACKAGES))

CFLAGS += -Wall -std=c99
OBJS = compton.o
MANPAGES = man/compton.1 man/compton-trans.1
MANPAGES_HTML = $(addsuffix .html,$(MANPAGES))

# === Recipes ===
.DEFAULT_GOAL := compton

%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) $(INCS) -c src/$*.c

compton: $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

man/%.1: man/%.1.asciidoc
	a2x --format manpage $<

man/%.1.html: man/%.1.asciidoc
	asciidoc $<

docs: $(MANPAGES) $(MANPAGES_HTML)

install: compton docs
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
	@rm -f $(OBJS) compton $(MANPAGES) $(MANPAGES_HTML)

.PHONY: uninstall clean docs
