PREFIX ?= /usr
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
	@cp -t $(PREFIX)/bin compton
	@[ -d "$(MANDIR)" ] \
	  && cp -t "$(MANDIR)" man/compton.1
	@cp -t $(PREFIX)/bin bin/settrans

uninstall:
	@rm -f $(PREFIX)/bin/compton
	@rm -f $(MANDIR)/compton.1
	@rm -f $(PREFIX)/bin/settrans

clean:
	@rm -f $(OBJS) compton

.PHONY: uninstall clean
