LIBS=`pkg-config --cflags --libs xcomposite xfixes xdamage xrender` -lm

CFLAGS=-O -g

xcompmgr: xcompmgr.c
	$(CC) -o $@ $(CFLAGS) xcompmgr.c $(LIBS)

clean:
	rm xcompmgr
