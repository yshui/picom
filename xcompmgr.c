#include <stdlib.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

typedef struct _win {
    struct _win		*next;
    Window		id;
    XWindowAttributes	a;
    int			damaged;
    int			mode;
    Damage		damage;
    Picture		picture;
    XserverRegion	borderSize;

    /* for drawing translucent windows */
    XserverRegion	borderClip;
    struct _win		*prev_trans;
} win;

win *list;

Display		*dpy;
int		scr;
Window		root;
Picture		rootPicture;
Picture		transPicture;
XserverRegion	allDamage;

#define WINDOW_PLAIN	0
#define WINDOW_DROP	1
#define WINDOW_TRANS	2

win *
find_win (Display *dpy, Window id)
{
    win	*w;

    for (w = list; w; w = w->next)
	if (w->id == id)
	    return w;
    return 0;
}

void
paint_root (Display *dpy)
{
    XRenderColor    c;
    
    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle (dpy, PictOpSrc, rootPicture, &c, 
			  0, 0, 32767, 32767);
}

XserverRegion
border_size (Display *dpy, win *w)
{
    XserverRegion   border;
    border = XFixesCreateRegionFromWindow (dpy, w->id, WindowRegionBounding);
    /* translate this */
    XFixesUnionRegion (dpy, border, border, w->a.x, w->a.y, None, 0, 0);
    return border;
}

void
paint_all (Display *dpy, XserverRegion region)
{
    win	*w;
    win	*t = 0;
    
    for (w = list; w; w = w->next)
    {
	Picture	mask;
	
	if (w->a.map_state != IsViewable)
	    continue;
	if (w->mode == WINDOW_TRANS)
	{
	    w->borderClip = XFixesCreateRegion (dpy, 0, 0);
	    XFixesUnionRegion (dpy, w->borderClip, region, 0, 0, None, 0, 0);
	    w->prev_trans = t;
	    t = w;
	}
	else
	{
	    XFixesSetPictureClipRegion (dpy, rootPicture, 0, 0, region);
	    if (w->borderSize)
		XFixesDestroyRegion (dpy, w->borderSize);
	    w->borderSize = border_size (dpy, w);
	    XFixesSubtractRegion (dpy, region, region, 0, 0, w->borderSize, 0, 0);
	    XRenderComposite (dpy, PictOpSrc, w->picture, None, rootPicture,
			      0, 0, 0, 0, 
			      w->a.x + w->a.border_width,
			      w->a.y + w->a.border_width,
			      w->a.width,
			      w->a.height);
	}
    }
    XFixesSetPictureClipRegion (dpy, rootPicture, 0, 0, region);
    paint_root (dpy);
    for (w = t; w; w = w->prev_trans)
    {
	XFixesSetPictureClipRegion (dpy, rootPicture, 0, 0, w->borderClip);
	XRenderComposite (dpy, PictOpOver, w->picture, transPicture, rootPicture,
			      0, 0, 0, 0, 
			      w->a.x + w->a.border_width,
			      w->a.y + w->a.border_width,
			      w->a.width,
			      w->a.height);
	XFixesDestroyRegion (dpy, w->borderClip);
	w->borderClip = None;
    }
    XFixesDestroyRegion (dpy, region);
}

void
add_damage (Display *dpy, XserverRegion damage)
{
    if (allDamage)
    {
	XFixesUnionRegion (dpy, allDamage, allDamage, 0, 0, damage, 0, 0);
	XFixesDestroyRegion (dpy, damage);
    }
    else
	allDamage = damage;
}

void
repair_win (Display *dpy, Window id)
{
    win		    *w = find_win (dpy, id);
    XserverRegion   parts;

    if (!w)
	return;
/*    printf ("repair 0x%x\n", w->id); */
    parts = XFixesCreateRegion (dpy, 0, 0);
    /* translate region */
    XDamageSubtract (dpy, w->damage, None, parts);
    XFixesUnionRegion (dpy, parts, parts, w->a.x, w->a.y, None, 0, 0);
    add_damage (dpy, parts);
}

void
map_win (Display *dpy, Window id)
{
    win		    *w = find_win (dpy, id);
    XserverRegion   region;

    if (!w)
	return;
    w->a.map_state = IsViewable;
    w->damage = XDamageCreate (dpy, id, XDamageReportNonEmpty);
    region = border_size (dpy, w);
    add_damage (dpy, region);
}

void
unmap_win (Display *dpy, Window id)
{
    win *w = find_win (dpy, id);

    if (!w)
	return;
    w->a.map_state = IsUnmapped;
    if (w->damage != None)
    {
	XDamageDestroy (dpy, w->damage);
	w->damage = None;
    }
    if (w->borderSize != None)
    {
	add_damage (dpy, w->borderSize);    /* destroys region */
	w->borderSize = None;
    }
}

void
add_win (Display *dpy, Window id, Window prev)
{
    win	*new = malloc (sizeof (win));
    win	**p;
    XWindowAttributes a;
    XRenderPictureAttributes pa;
    
    if (!new)
	return;
    if (prev)
    {
	for (p = &list; *p; p = &(*p)->next)
	    if ((*p)->id == prev)
		break;
    }
    else
	p = &list;
    new->id = id;
    if (!XGetWindowAttributes (dpy, id, &new->a))
    {
	free (new);
	return;
    }
    new->damaged = 0;
    new->damage = None;
    pa.subwindow_mode = IncludeInferiors;
    new->picture = XRenderCreatePicture (dpy, id,
					 XRenderFindVisualFormat (dpy, 
								  new->a.visual),
					 CPSubwindowMode,
					 &pa);
					 
    new->borderSize = None;
    if (new->a.override_redirect)
	new->mode = WINDOW_TRANS;
    else
	new->mode = WINDOW_DROP;
    new->next = *p;
    *p = new;
    if (new->a.map_state == IsViewable)
	map_win (dpy, id);
}

void
configure_win (Display *dpy, XConfigureEvent *ce)
{
    win		    *w = find_win (dpy, ce->window);
    Window	    above;
    XserverRegion   damage = None;
    
    if (!w)
	return;
    if (w->a.map_state == IsViewable)
    {
	damage = XFixesCreateRegion (dpy, 0, 0);
	if (w->borderSize != None)	
	    XFixesUnionRegion (dpy, damage, w->borderSize, 0, 0, None, 0, 0);
    }
    w->a.x = ce->x;
    w->a.y = ce->y;
    w->a.width = ce->width;
    w->a.height = ce->height;
    w->a.border_width = ce->border_width;
    w->a.override_redirect = ce->override_redirect;
    if (w->next)
	above = w->next->id;
    else
	above = None;
    if (above != ce->above)
    {
	win **prev;

	/* unhook */
	for (prev = &list; *prev; prev = &(*prev)->next)
	    if ((*prev) == w)
		break;
	*prev = w->next;
	
	/* rehook */
	for (prev = &list; *prev; prev = &(*prev)->next)
	{
	    if ((*prev)->id == ce->above)
		break;
	}
	w->next = *prev;
	*prev = w;
    }
    if (damage)
    {
	XserverRegion	border = border_size (dpy, w);
	XFixesUnionRegion (dpy, damage, damage, 0, 0, border, 0, 0);
	add_damage (dpy, damage);
    }
}

void
destroy_win (Display *dpy, Window id, Bool gone)
{
    win	**prev, *w;

    for (prev = &list; w = *prev; prev = &w->next)
	if (w->id == id)
	{
	    if (!gone)
	    {
		unmap_win (dpy, id);
		XRenderFreePicture (dpy, w->picture);
	    }
	    *prev = w->next;
	    free (w);
	    break;
	}
}

void
dump_win (win *w)
{
    printf ("\t%08x: %d x %d + %d + %d (%d)\n", w->id,
	    w->a.width, w->a.height, w->a.x, w->a.y, w->a.border_width);
}

void
dump_wins (void)
{
    win	*w;

    printf ("windows:\n");
    for (w = list; w; w = w->next)
	dump_win (w);
}

void
damage_win (Display *dpy, XDamageNotifyEvent *de)
{
    repair_win (dpy, de->drawable);
}

int
error (Display *dpy, XErrorEvent *ev)
{
    printf ("error %d request %d minor %d\n",
	    ev->error_code, ev->request_code, ev->minor_code);
}

void
expose_root (Display *dpy, Window root, XRectangle *rects, int nrects)
{
    XserverRegion  region = XFixesCreateRegion (dpy, rects, nrects);
    
    add_damage (dpy, region);
}

main ()
{
    XEvent	    ev;
    int		    event_base, error_base;
    Window	    root_return, parent_return;
    Window	    *children;
    Pixmap	    transPixmap;
    unsigned int    nchildren;
    int		    i;
    int		    damage_event, damage_error;
    int		    xfixes_event, xfixes_error;
    XRenderPictureAttributes	pa;
    XRenderColor		c;
    XRectangle	    *expose_rects = 0;
    int		    size_expose = 0;
    int		    n_expose = 0;

    dpy = XOpenDisplay (0);
    if (!dpy)
    {
	fprintf (stderr, "Can't open display\n");
	exit (1);
    }
    XSetErrorHandler (error);
    scr = DefaultScreen (dpy);
    root = RootWindow (dpy, scr);
    pa.subwindow_mode = IncludeInferiors;
    transPixmap = XCreatePixmap (dpy, root, 1, 1, 8);
    pa.repeat = True;
    transPicture = XRenderCreatePicture (dpy, transPixmap,
					 XRenderFindStandardFormat (dpy, PictStandardA8),
					 CPRepeat,
					 &pa);
    c.red = c.green = c.blue = 0;
    c.alpha = 0xc0c0;
    XRenderFillRectangle (dpy, PictOpSrc, transPicture, &c, 0, 0, 1, 1);
    
    rootPicture = XRenderCreatePicture (dpy, root, 
					XRenderFindVisualFormat (dpy,
								 DefaultVisual (dpy, scr)),
					CPSubwindowMode,
					&pa);
    if (!XCompositeQueryExtension (dpy, &event_base, &error_base))
    {
	fprintf (stderr, "No composite extension\n");
	exit (1);
    }
    if (!XDamageQueryExtension (dpy, &damage_event, &damage_error))
    {
	fprintf (stderr, "No damage extension\n");
	exit (1);
    }
    if (!XFixesQueryExtension (dpy, &xfixes_event, &xfixes_error))
    {
	fprintf (stderr, "No XFixes extension\n");
	exit (1);
    }
    allDamage = None;
    XGrabServer (dpy);
    XCompositeRedirectSubwindows (dpy, root, CompositeRedirectManual);
    paint_root (dpy);
    XSelectInput (dpy, root, SubstructureNotifyMask|ExposureMask);
    XQueryTree (dpy, root, &root_return, &parent_return, &children, &nchildren);
    for (i = 0; i < nchildren; i++)
	add_win (dpy, children[i], i ? children[i-1] : None);
    XFree (children);
    XUngrabServer (dpy);
    for (;;)
    {
/*	dump_wins (); */
	do {
	    XNextEvent (dpy, &ev);
/*	    printf ("event %d\n", ev.type); */
	    switch (ev.type) {
	    case CreateNotify:
		add_win (dpy, ev.xcreatewindow.window, 0);
		break;
	    case ConfigureNotify:
		configure_win (dpy, &ev.xconfigure);
		break;
	    case DestroyNotify:
		destroy_win (dpy, ev.xdestroywindow.window, True);
		break;
	    case MapNotify:
		map_win (dpy, ev.xmap.window);
		break;
	    case UnmapNotify:
		unmap_win (dpy, ev.xunmap.window);
		break;
	    case ReparentNotify:
		if (ev.xreparent.parent == root)
		    add_win (dpy, ev.xreparent.window, 0);
		else
		    destroy_win (dpy, ev.xreparent.window, False);
		break;
	    case Expose:
		if (ev.xexpose.window == root)
		{
		    int more = ev.xexpose.count + 1;
		    if (n_expose == size_expose)
		    {
			if (expose_rects)
			{
			    expose_rects = realloc (expose_rects, 
						    (size_expose + more) * 
						    sizeof (XRectangle));
			    size_expose += more;
			}
			else
			{
			    expose_rects = malloc (more * sizeof (XRectangle));
			    size_expose = more;
			}
		    }
		    expose_rects[n_expose].x = ev.xexpose.x;
		    expose_rects[n_expose].y = ev.xexpose.y;
		    expose_rects[n_expose].width = ev.xexpose.width;
		    expose_rects[n_expose].height = ev.xexpose.height;
		    n_expose++;
		    if (ev.xexpose.count == 0)
		    {
			expose_root (dpy, root, expose_rects, n_expose);
			n_expose = 0;
		    }
		}
		break;
	    default:
		if (ev.type == damage_event + XDamageNotify)
		    damage_win (dpy, (XDamageNotifyEvent *) &ev);
		break;
	    }
	} while (XEventsQueued (dpy, QueuedAfterReading));
	if (allDamage)
	{
	    paint_all (dpy, allDamage);
	    allDamage = None;
	}
    }
}
