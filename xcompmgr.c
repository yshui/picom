/*
 * $Id$
 *
 * Copyright © 2003 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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
    XserverRegion	extents;
    Picture		shadow;
    int			shadow_dx;
    int			shadow_dy;
    int			shadow_width;
    int			shadow_height;

    /* for drawing translucent windows */
    XserverRegion	borderClip;
    struct _win		*prev_trans;
} win;

typedef struct _conv {
    int	    size;
    double  *data;
} conv;

win             *list;
Display		*dpy;
int		scr;
Window		root;
Picture		rootPicture;
Picture		rootBuffer;
Picture		transPicture;
Picture		blackPicture;
Picture		rootTile;
XserverRegion	allDamage;
int		root_height, root_width;
conv            *gussianMap;

#define BACKGROUND_PROP	"_XROOTPMAP_ID"

#define WINDOW_SOLID	0
#define WINDOW_TRANS	1
#define WINDOW_ARGB	2

#define TRANS_OPACITY	0.75
#define SHADOW_RADIUS	15
#define SHADOW_OPACITY	0.75
#define SHADOW_OFFSET_X	(-SHADOW_RADIUS)
#define SHADOW_OFFSET_Y	(-SHADOW_RADIUS)


static double
gaussian (double r, double x, double y)
{
    return ((1 / (sqrt (2 * M_PI * r))) *
	    exp ((- (x * x + y * y)) / (2 * r * r)));
}


static conv *
make_gaussian_map (Display *dpy, double r)
{
    conv	    *c;
    int		    size = ((int) ceil ((r * 3)) + 1) & ~1;
    int		    center = size / 2;
    int		    x, y;
    double	    t;
    double	    g;
    
    c = malloc (sizeof (conv) + size * size * sizeof (double));
    c->size = size;
    c->data = (double *) (c + 1);
    for (y = 0; y < size; y++)
	for (x = 0; x < size; x++)
	{
	    g = gaussian (r, (double) (x - center), (double) (y - center));
	    t += g;
	    c->data[y * size + x] = g;
	}
/*    printf ("gaussian total %f\n", t); */
    for (y = 0; y < size; y++)
	for (x = 0; x < size; x++)
	{
	    c->data[y*size + x] /= t;
	}
    return c;
}

/*
 * A picture will help
 *
 *	-center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *	    |     |                   |     |
 *	    |     |                   |     |
 *        0 +-----+-------------------+-----+
 *	    |     |                   |     |
 *	    |     |                   |     |
 *	    |     |                   |     |
 *   height +-----+-------------------+-----+
 *	    |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */
 
static unsigned char
sum_gaussian (conv *map, double opacity, int x, int y, int width, int height)
{
    int	    fx, fy;
    double  *g_data;
    double  *g_line = map->data;
    int	    g_size = map->size;
    int	    center = g_size / 2;
    int	    fx_start, fx_end;
    int	    fy_start, fy_end;
    double  v;
    
    /*
     * Compute set of filter values which are "in range",
     * that's the set with:
     *	0 <= x + (fx-center) && x + (fx-center) < width &&
     *  0 <= y + (fy-center) && y + (fy-center) < height
     *
     *  0 <= x + (fx - center)	x + fx - center < width
     *  center - x <= fx	fx < width + center - x
     */

    fx_start = center - x;
    if (fx_start < 0)
	fx_start = 0;
    fx_end = width + center - x;
    if (fx_end > g_size)
	fx_end = g_size;

    fy_start = center - y;
    if (fy_start < 0)
	fy_start = 0;
    fy_end = height + center - y;
    if (fy_end > g_size)
	fy_end = g_size;

    g_line = g_line + fy_start * g_size + fx_start;
    
    v = 0;
    for (fy = fy_start; fy < fy_end; fy++)
    {
	g_data = g_line;
	g_line += g_size;
	
	for (fx = fx_start; fx < fx_end; fx++)
	    v += *g_data++;
    }
    if (v > 1)
	v = 1;
    
    return ((unsigned int) (v * opacity * 255.0));
}

static XImage *
make_shadow (Display *dpy, double opacity, int width, int height)
{
    XImage	    *ximage;
    unsigned char   *data;
    int		    gsize = gussianMap->size;
    int		    ylimit, xlimit;
    int		    swidth = width + gsize;
    int		    sheight = height + gsize;
    int		    center = gsize / 2;
    int		    x, y;
    unsigned char   d;
    
    data = malloc (swidth * sheight * sizeof (unsigned char));
    ximage = XCreateImage (dpy,
			   DefaultVisual(dpy, DefaultScreen(dpy)),
			   8,
			   ZPixmap,
			   0,
			   (char *) data,
			   swidth, sheight, 8, swidth * sizeof (unsigned char));
    /*
     * Build the gaussian in sections
     */

    /*
     * corners
     */
    ylimit = gsize;
    if (ylimit > sheight / 2)
	ylimit = (sheight + 1) / 2;
    xlimit = gsize;
    if (xlimit > swidth / 2)
	xlimit = (swidth + 1) / 2;

    for (y = 0; y < ylimit; y++)
	for (x = 0; x < xlimit; x++)
	{
	    d = sum_gaussian (gussianMap, opacity, x - center, y - center, width, height);
	    data[y * swidth + x] = d;
	    data[(sheight - y - 1) * swidth + x] = d;
	    data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
	    data[y * swidth + (swidth - x - 1)] = d;
	}

    /*
     * top/bottom
     */
    for (y = 0; y < ylimit; y++)
    {
	d = sum_gaussian (gussianMap, opacity, center, y - center, width, height);
	for (x = gsize; x < swidth - gsize; x++)
	{
	    data[y * swidth + x] = d;
	    data[(sheight - y - 1) * swidth + x] = d;
	}
    }

    /*
     * sides
     */
    
    for (x = 0; x < xlimit; x++)
    {
	d = sum_gaussian (gussianMap, opacity, x - center, center, width, height);
	for (y = gsize; y < sheight - gsize; y++)
	{
	    data[y * swidth + x] = d;
	    data[y * swidth + (swidth - x - 1)] = d;
	}
    }

    /*
     * center
     */

    d = sum_gaussian (gussianMap, opacity, center, center, width, height);
    for (y = ylimit; y < sheight - ylimit; y++)
	for (x = xlimit; x < swidth - xlimit; x++)
	    data[y * swidth + x] = d;

    return ximage;
}

static Picture
shadow_picture (Display *dpy, double opacity, int width, int height, int *wp, int *hp)
{
    XImage  *shadowImage = make_shadow (dpy, opacity, width, height);
    Pixmap  shadowPixmap = XCreatePixmap (dpy, root, 
					  shadowImage->width,
					  shadowImage->height,
					  8);
    Picture shadowPicture = XRenderCreatePicture (dpy, shadowPixmap,
						  XRenderFindStandardFormat (dpy, PictStandardA8),
						  0, 0);
    GC	    gc = XCreateGC (dpy, shadowPixmap, 0, 0);
    
    XPutImage (dpy, shadowPixmap, gc, shadowImage, 0, 0, 0, 0, 
	       shadowImage->width,
	       shadowImage->height);
    *wp = shadowImage->width;
    *hp = shadowImage->height;
    XFreeGC (dpy, gc);
    XDestroyImage (shadowImage);
    XFreePixmap (dpy, shadowPixmap);
    return shadowPicture;
}

static win *
find_win (Display *dpy, Window id)
{
    win	*w;

    for (w = list; w; w = w->next)
	if (w->id == id)
	    return w;
    return 0;
}

static Picture
root_tile (Display *dpy)
{
    Picture	    picture;
    Atom	    actual_type;
    Pixmap	    pixmap;
    int		    actual_format;
    unsigned long   nitems;
    unsigned long   bytes_after;
    unsigned char   *prop;
    Bool	    fill;
    XRenderPictureAttributes	pa;

    if (XGetWindowProperty (dpy, root, XInternAtom (dpy, BACKGROUND_PROP, False),
			    0, 4, False, AnyPropertyType,
			    &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success &&
	actual_type == XInternAtom (dpy, "PIXMAP", False) && actual_format == 32 && nitems == 1)
    {
	memcpy (&pixmap, prop, 4);
	XFree (prop);
	fill = False;
    }
    else
    {
	pixmap = XCreatePixmap (dpy, root, 1, 1, DefaultDepth (dpy, scr));
	fill = True;
    }
    pa.repeat = True;
    picture = XRenderCreatePicture (dpy, pixmap,
				    XRenderFindVisualFormat (dpy,
							     DefaultVisual (dpy, scr)),
				    CPRepeat, &pa);
    if (fill)
    {
	XRenderColor    c;
	
	c.red = c.green = c.blue = 0x8080;
	c.alpha = 0xffff;
	XRenderFillRectangle (dpy, PictOpSrc, picture, &c, 
			      0, 0, 1, 1);
    }
    return picture;
}

static void
paint_root (Display *dpy)
{
    if (!rootTile)
	rootTile = root_tile (dpy);
    
    XRenderComposite (dpy, PictOpSrc,
		      rootTile, None, rootBuffer,
		      0, 0, 0, 0, 0, 0, root_width, root_height);
}

static XserverRegion
win_extents (Display *dpy, win *w)
{
    XRectangle	    r;
    
    if (w->mode == WINDOW_ARGB)
    {
	r.x = w->a.x;
	r.y = w->a.y;
	r.width = w->a.width + w->a.border_width * 2;
	r.height = w->a.height + w->a.border_width * 2;
    }
    else
    {
	if (!w->shadow)
	{
	    double	opacity = SHADOW_OPACITY;
	    if (w->mode == WINDOW_TRANS)
		opacity = opacity * TRANS_OPACITY;
	    w->shadow = shadow_picture (dpy, opacity, 
					w->a.width, w->a.height,
					&w->shadow_width, &w->shadow_height);
	    w->shadow_dx = SHADOW_OFFSET_X;
	    w->shadow_dy = SHADOW_OFFSET_Y;
	}
	r.x = w->a.x + w->a.border_width + w->shadow_dx;
	r.y = w->a.y + w->a.border_width + w->shadow_dy;
	r.width = w->shadow_width;
	r.height = w->shadow_height;
    }
    return XFixesCreateRegion (dpy, &r, 1);
}

static XserverRegion
border_size (Display *dpy, win *w)
{
    XserverRegion   border;
    border = XFixesCreateRegionFromWindow (dpy, w->id, WindowRegionBounding);
    XFixesTranslateRegion (dpy, border, w->a.x, w->a.y);
    return border;
}

static void
paint_all (Display *dpy, XserverRegion region)
{
    win	*w;
    win	*t = 0;
    
    if (!region)
    {
	XRectangle  r;
	r.x = 0;
	r.y = 0;
	r.width = root_width;
	r.height = root_height;
	region = XFixesCreateRegion (dpy, &r, 1);
    }
    if (!rootBuffer)
    {
	Pixmap	rootPixmap = XCreatePixmap (dpy, root, root_width, root_height,
					    DefaultDepth (dpy, scr));
	rootBuffer = XRenderCreatePicture (dpy, rootPixmap,
					   XRenderFindVisualFormat (dpy,
								    DefaultVisual (dpy, scr)),
					   0, 0);
	XFreePixmap (dpy, rootPixmap);
    }
    XFixesSetPictureClipRegion (dpy, rootPicture, 0, 0, region);
    for (w = list; w; w = w->next)
    {
	if (w->a.map_state != IsViewable)
	    continue;
	if (!w->picture)
	    continue;
	
	if (w->borderSize)
	    XFixesDestroyRegion (dpy, w->borderSize);
	w->borderSize = border_size (dpy, w);
	if (w->extents)
	    XFixesDestroyRegion (dpy, w->extents);
	w->extents = win_extents (dpy, w);
	if (w->mode == WINDOW_SOLID)
	{
	    XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, region);
	    XFixesSubtractRegion (dpy, region, region, w->borderSize);
	    XRenderComposite (dpy, PictOpSrc, w->picture, None, rootBuffer,
			      0, 0, 0, 0, 
			      w->a.x + w->a.border_width,
			      w->a.y + w->a.border_width,
			      w->a.width,
			      w->a.height);
	}
	w->borderClip = XFixesCreateRegion (dpy, 0, 0);
	XFixesCopyRegion (dpy, w->borderClip, region);
	w->prev_trans = t;
	t = w;
    }
    XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, region);
    paint_root (dpy);
    for (w = t; w; w = w->prev_trans)
    {
	XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, w->borderClip);
	if (w->shadow)
	{
	    XRenderComposite (dpy, PictOpOver, blackPicture, w->shadow, rootBuffer,
			      0, 0, 0, 0,
			      w->a.x + w->a.border_width + w->shadow_dx,
			      w->a.y + w->a.border_width + w->shadow_dy,
			      w->shadow_width, w->shadow_height);
	}
	if (w->mode == WINDOW_TRANS)
	    XRenderComposite (dpy, PictOpOver, w->picture, transPicture, rootBuffer,
			      0, 0, 0, 0, 
			      w->a.x + w->a.border_width,
			      w->a.y + w->a.border_width,
			      w->a.width,
			      w->a.height);
	else if (w->mode == WINDOW_ARGB)
	    XRenderComposite (dpy, PictOpOver, w->picture, None, rootBuffer,
			      0, 0, 0, 0, 
			      w->a.x + w->a.border_width,
			      w->a.y + w->a.border_width,
			      w->a.width,
			      w->a.height);
	XFixesDestroyRegion (dpy, w->borderClip);
	w->borderClip = None;
    }
    XFixesDestroyRegion (dpy, region);
    XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, None);
    XRenderComposite (dpy, PictOpSrc, rootBuffer, None, rootPicture,
		      0, 0, 0, 0, 0, 0, root_width, root_height);
}

static void
add_damage (Display *dpy, XserverRegion damage)
{
    if (allDamage)
    {
	XFixesUnionRegion (dpy, allDamage, allDamage, damage);
	XFixesDestroyRegion (dpy, damage);
    }
    else
	allDamage = damage;
}

static void
repair_win (Display *dpy, Window id)
{
    win		    *w = find_win (dpy, id);
    XserverRegion   parts;

    if (!w)
	return;
/*    printf ("repair 0x%x\n", w->id); */
    parts = XFixesCreateRegion (dpy, 0, 0);
    XDamageSubtract (dpy, w->damage, None, parts);
    XFixesTranslateRegion (dpy, parts, w->a.x, w->a.y);
    add_damage (dpy, parts);
}

static void
map_win (Display *dpy, Window id)
{
    win		    *w = find_win (dpy, id);
    XserverRegion   region;

    if (!w)
	return;
    w->a.map_state = IsViewable;
    if (w->picture)
    {
	w->damage = XDamageCreate (dpy, id, XDamageReportNonEmpty);
	region = win_extents (dpy, w);
	add_damage (dpy, region);
    }
}

static void
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
    if (w->extents != None)
    {
	add_damage (dpy, w->extents);    /* destroys region */
	w->extents = None;
    }
}

static void
add_win (Display *dpy, Window id, Window prev)
{
    win				*new = malloc (sizeof (win));
    win				**p;
    XRenderPictureAttributes	pa;
    XRenderPictFormat		*format;
    
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
    if (new->a.class == InputOnly)
    {
	new->picture = 0;
	format = 0;
    }
    else
    {
	format = XRenderFindVisualFormat (dpy, new->a.visual);
	new->picture = XRenderCreatePicture (dpy, id,
					     format,
					     CPSubwindowMode,
					     &pa);
    }
					 
    new->shadow = None;
    new->borderSize = None;
    new->extents = None;
    if (format && format->type == PictTypeDirect && format->direct.alphaMask)
	new->mode = WINDOW_ARGB;
    else if (new->a.override_redirect)
	new->mode = WINDOW_TRANS;
    else
	new->mode = WINDOW_SOLID;
    new->next = *p;
    *p = new;
    if (new->a.map_state == IsViewable)
	map_win (dpy, id);
}

static void
configure_win (Display *dpy, XConfigureEvent *ce)
{
    win		    *w = find_win (dpy, ce->window);
    Window	    above;
    XserverRegion   damage = None;
    
    if (!w)
    {
	if (ce->window == root)
	{
	    if (rootBuffer)
	    {
		XRenderFreePicture (dpy, rootBuffer);
		rootBuffer = None;
	    }
	    root_width = ce->width;
	    root_height = ce->height;
	}
	return;
    }
    if (w->a.map_state == IsViewable)
    {
	damage = XFixesCreateRegion (dpy, 0, 0);
	if (w->extents != None)	
	    XFixesCopyRegion (dpy, damage, w->extents);
    }
    w->a.x = ce->x;
    w->a.y = ce->y;
    if (w->a.width != ce->width || w->a.height != ce->height)
	if (w->shadow)
	{
	    XRenderFreePicture (dpy, w->shadow);
	    w->shadow = None;
	}
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
	XserverRegion	extents = win_extents (dpy, w);
	XFixesUnionRegion (dpy, damage, damage, extents);
	XFixesDestroyRegion (dpy, extents);
	add_damage (dpy, damage);
    }
}

static void
destroy_win (Display *dpy, Window id, Bool gone)
{
    win	**prev, *w;

    for (prev = &list; (w = *prev); prev = &w->next)
	if (w->id == id)
	{
	    if (!gone)
	    {
		unmap_win (dpy, id);
		if (w->picture)
		    XRenderFreePicture (dpy, w->picture);
	    }
	    *prev = w->next;
	    free (w);
	    break;
	}
}

/*
static void
dump_win (win *w)
{
    printf ("\t%08lx: %d x %d + %d + %d (%d)\n", w->id,
	    w->a.width, w->a.height, w->a.x, w->a.y, w->a.border_width);
}


static void
dump_wins (void)
{
    win	*w;

    printf ("windows:\n");
    for (w = list; w; w = w->next)
	dump_win (w);
}
*/

static void
damage_win (Display *dpy, XDamageNotifyEvent *de)
{
    repair_win (dpy, de->drawable);
}

static int
error (Display *dpy, XErrorEvent *ev)
{
    printf ("error %d request %d minor %d\n",
	    ev->error_code, ev->request_code, ev->minor_code);

    return 0;
}

static void
expose_root (Display *dpy, Window root, XRectangle *rects, int nrects)
{
    XserverRegion  region = XFixesCreateRegion (dpy, rects, nrects);
    
    add_damage (dpy, region);
}

#define INTERVAL    0

#if INTERVAL
static int
time_in_millis (void)
{
    struct timeval  tp;

    gettimeofday (&tp, 0);
    return(tp.tv_sec * 1000) + (tp.tv_usec / 1000);
}
#endif

int
main (void)
{
    XEvent	    ev;
    int		    event_base, error_base;
    Window	    root_return, parent_return;
    Window	    *children;
    Pixmap	    transPixmap;
    Pixmap	    blackPixmap;
    unsigned int    nchildren;
    int		    i;
    int		    damage_event, damage_error;
    int		    xfixes_event, xfixes_error;
    XRenderPictureAttributes	pa;
    XRenderColor		c;
    XRectangle	    *expose_rects = 0;
    int		    size_expose = 0;
    int		    n_expose = 0;
#if INTERVAL
    int		    timeout;
#endif

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

    gussianMap = make_gaussian_map(dpy, SHADOW_RADIUS);

    transPixmap = XCreatePixmap (dpy, root, 1, 1, 8);
    pa.repeat = True;
    transPicture = XRenderCreatePicture (dpy, transPixmap,
					 XRenderFindStandardFormat (dpy, PictStandardA8),
					 CPRepeat,
					 &pa);
    c.red = c.green = c.blue = 0;
    c.alpha = 0xc0c0;
    XRenderFillRectangle (dpy, PictOpSrc, transPicture, &c, 0, 0, 1, 1);
    
    root_width = DisplayWidth (dpy, scr);
    root_height = DisplayHeight (dpy, scr);
    
    rootPicture = XRenderCreatePicture (dpy, root, 
					XRenderFindVisualFormat (dpy,
								 DefaultVisual (dpy, scr)),
					CPSubwindowMode,
					&pa);
    blackPixmap = XCreatePixmap (dpy, root, 1, 1, 32);
    pa.repeat = True;
    blackPicture = XRenderCreatePicture (dpy, blackPixmap,
					 XRenderFindStandardFormat (dpy, PictStandardARGB32),
					 CPRepeat,
					 &pa);
    c.red = c.green = c.blue = 0;
    c.alpha = 0xffff;
    XRenderFillRectangle (dpy, PictOpSrc, blackPicture, &c, 0, 0, 1, 1);
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
    XSelectInput (dpy, root, 
		  SubstructureNotifyMask|
		  ExposureMask|
		  StructureNotifyMask|
		  PropertyChangeMask);
    XQueryTree (dpy, root, &root_return, &parent_return, &children, &nchildren);
    for (i = 0; i < nchildren; i++)
	add_win (dpy, children[i], i ? children[i-1] : None);
    XFree (children);
    XUngrabServer (dpy);
    paint_all (dpy, None);
#if INTERVAL
    last_update = time_in_millis ();
#endif
    for (;;)
    {
#if INTERVAL
	int busy_start = 0;
#endif
/*	dump_wins (); */
	do {
	    XNextEvent (dpy, &ev);
#if INTERVAL
	    if (!busy_start)
		busy_start = time_in_millis();
#endif
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
	    case PropertyNotify:
		if (ev.xproperty.atom == XInternAtom (dpy, BACKGROUND_PROP, False))
		{
		    if (rootTile)
		    {
			XClearArea (dpy, root, 0, 0, 0, 0, True);
			XRenderFreePicture (dpy, rootTile);
			rootTile = None;
		    }
		}
		break;
	    default:
		if (ev.type == damage_event + XDamageNotify)
		    damage_win (dpy, (XDamageNotifyEvent *) &ev);
		break;
	    }
	} while (XEventsQueued (dpy, QueuedAfterReading));
#if INTERVAL
	now = time_in_millis ();
/*	printf ("\t\tbusy %d\n", now - busy_start); */
	timeout = INTERVAL - (now - last_update);
	if (timeout > 0)
	{
	    ufd.fd = ConnectionNumber (dpy);
	    ufd.events = POLLIN;
	    n = poll (&ufd, 1, timeout);
	    if (n > 0 && (ufd.revents & POLLIN) && XEventsQueued (dpy, QueuedAfterReading))
		continue;
	}
#endif
	if (allDamage)
	{
#if INTERVAL
	    int	old_update = last_update;
	    last_update = time_in_millis();
/*	    printf ("delta %d\n", last_update - old_update); */
#endif
	    paint_all (dpy, allDamage);
	    allDamage = None;
	}
    }
}
