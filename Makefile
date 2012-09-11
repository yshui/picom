CC ?= gcc

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

PACKAGES = x11 xcomposite xfixes xdamage xrender xext
LIBS = $(shell pkg-config --libs $(PACKAGES)) -lm
INCS = $(shell pkg-config --cflags $(PACKAGES))
CFLAGS += -Wall
OBJS = compton.o

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
