LIBS=`pkg-config --cflags --libs xcomposite xfixes xdamage xrender`

xcompmgr: xcompmgr.c
	$(CC) -o $@ $(CFLAGS) xcompmgr.c $(LIBS)

clean:
	rm xcompmgr
