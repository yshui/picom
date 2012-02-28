PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs $(PACKAGES)` -lm
INCS = `pkg-config --cflags $(PACKAGES)`
CFLAGS = -Wall
OBJS = compton.o

%.o: src/%.c src/%.h
	$(CC) $(CFLAGS) $(INCS) -c src/$*.c

compton: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

install: compton
	@[ -e "$(PREFIX)" ] || mkdir -p "$(PREFIX)"
	@[ -e "$(BINDIR)" ] || mkdir -p "$(BINDIR)"
	@[ -e "$(MANDIR)" ] || mkdir -p "$(MANDIR)"
	@cp compton "$(BINDIR)"
	@cp bin/settrans "$(BINDIR)"
	@cp man/compton.1 "$(MANDIR)"

uninstall:
	@rm -f "$(BINDIR)/compton"
	@rm -f "$(BINDIR)/settrans"
	@rm -f "$(MANDIR)/compton.1"

clean:
	@rm -f $(OBJS) compton

.PHONY: uninstall clean
