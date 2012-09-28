CC ?= gcc

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

PACKAGES = x11 xcomposite xfixes xdamage xrender xext libconfig
LIBS = $(shell pkg-config --libs $(PACKAGES)) -lm
LIBS += $(shell pcre-config --libs)
INCS = $(shell pkg-config --cflags $(PACKAGES))
INCS += $(shell pcre-config --cflags)
CFLAGS += -Wall -std=c99
OBJS = compton.o

CFG ?= -DCONFIG_LIBCONFIG -DCONFIG_REGEX_PCRE -DCONFIG_REGEX_PCRE_JIT
# libconfig-1.3* does not define LIBCONFIG_VER* macros, so we use pkg-config
# to determine its version here
CFG += $(shell pkg-config --atleast-version=1.4 libconfig || echo '-DCONFIG_LIBCONFIG_LEGACY')
CFLAGS += $(CFG)

%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) $(INCS) -c src/$*.c

compton: $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

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

.PHONY: uninstall clean
