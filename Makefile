PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs $(PACKAGES)` -lm
INCS = `pkg-config --cflags $(PACKAGES)`
CFLAGS = -Wall
PREFIX = /usr
MANDIR = $(PREFIX)/share/man/man1
OBJS = compton.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

compton: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJS): compton.h

install: compton
	@cp -t $(PREFIX)/bin compton
	@[ -d "$(MANDIR)" ] \
	  && cp -t "$(MANDIR)" compton.1
	@cp -t $(PREFIX)/bin settrans

uninstall:
	@rm -f $(PREFIX)/bin/compton
	@rm -f $(MANDIR)/compton.1
	@rm -f $(PREFIX)/bin/settrans

clean:
	@rm -f $(OBJS) compton

.PHONY: uninstall clean
