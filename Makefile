LIBS=`pkg-config --cflags --libs xcomposite xfixes xdamage xrender` -lm

CFLAGS=-O -g

all: xcompmgr mbcompmgr

xcompmgr: xcompmgr.c
	$(CC) -o $@ $(CFLAGS) xcompmgr.c $(LIBS)

mbcompmgr: mbcompmgr.c
	$(CC) -o $@ $(CFLAGS) mbcompmgr.c $(LIBS)

clean:
	rm xcompmgr mbcompmgr
