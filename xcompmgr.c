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


/* Modified by Matthew Hawn. I don't know what to say here so follow what it 
   says above. Not that I can really do anything about it
*/


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#if COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2
#define HAS_NAME_WINDOW_PIXMAP 1
#endif

#define CAN_DO_USABLE 0

typedef struct _ignore {
    struct _ignore	*next;
    unsigned long	sequence;
} ignore;

typedef struct _win {
    struct _win		*next;
    Window		id;
#if HAS_NAME_WINDOW_PIXMAP
    Pixmap		pixmap;
#endif
    XWindowAttributes	a;
#if CAN_DO_USABLE
    Bool		usable;		    /* mapped and all damaged at one point */
    XRectangle		damage_bounds;	    /* bounds of damage */
#endif
    int			mode;
    int			damaged;
    Damage		damage;
    Picture		picture;
    Picture		alphaPict;
    Picture		shadowPict;
    XserverRegion	borderSize;
    XserverRegion	extents;
    Picture		shadow;
    int			shadow_dx;
    int			shadow_dy;
    int			shadow_width;
    int			shadow_height;
    unsigned int	opacity;

    unsigned long	damage_sequence;    /* sequence when damage was created */

    /* for drawing translucent windows */
    XserverRegion	borderClip;
    struct _win		*prev_trans;
} win;

typedef struct _conv {
    int	    size;
    double  *data;
} conv;

typedef struct _fade {
    struct _fade	*next;
    win			*w;
    double		cur;
    double		step;
    void		(*callback) (Display *dpy, win *w, Bool gone);
    Display		*dpy;
    Bool		gone;
} fade;

win             *list;
fade		*fades;
Display		*dpy;
int		scr;
Window		root;
Picture		rootPicture;
Picture		rootBuffer;
Picture		blackPicture;
Picture		transBlackPicture;
Picture		rootTile;
XserverRegion	allDamage;
Bool		clipChanged;
#if HAS_NAME_WINDOW_PIXMAP
Bool		hasNamePixmap;
#endif
int		root_height, root_width;
ignore		*ignore_head, **ignore_tail = &ignore_head;
int		xfixes_event, xfixes_error;
int		damage_event, damage_error;
int		composite_event, composite_error;
int		render_event, render_error;
Bool		synchronize;
int		composite_opcode;

/* find these once and be done with it */
Atom		opacityAtom;

/* opacity property name; sometime soon I'll write up an EWMH spec for it */
#define OPACITY_PROP	"_NET_WM_WINDOW_OPACITY"

#define TRANSLUCENT	0xe0000000
#define OPAQUE		0xffffffff

conv            *gaussianMap;

#define WINDOW_SOLID	0
#define WINDOW_TRANS	1
#define WINDOW_ARGB	2

#define TRANS_OPACITY	0.75

#define DEBUG_REPAINT 0
#define DEBUG_EVENTS 0
#define MONITOR_REPAINT 0

#define SHADOWS		1
#define SHARP_SHADOW	0

typedef enum _compMode {
    CompSimple,		/* looks like a regular X server */
    CompServerShadows,	/* use window alpha for shadow; sharp, but precise */
    CompClientShadows,	/* use window extents for shadow, blurred */
} CompMode;

static void
determine_mode(Display *dpy, win *w);
    
CompMode    compMode = CompSimple;

int	    shadowRadius = 12;

double	fade_step =	0.05;
int	fade_delta =	10;
int	fade_time =	0;
Bool	fadeWindows;

Bool	autoRedirect = False;

int
get_time_in_milliseconds ()
{
    struct timeval  tv;

    gettimeofday (&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

fade *
find_fade (win *w)
{
    fade    *f;
    
    for (f = fades; f; f = f->next)
    {
	if (f->w == w)
	    return f;
    }
    return 0;
}

void
dequeue_fade (Display *dpy, fade *f)
{
    fade    **prev;

    for (prev = &fades; *prev; prev = &(*prev)->next)
	if (*prev == f)
	{
	    *prev = f->next;
	    if (f->callback)
		(*f->callback) (dpy, f->w, f->gone);
	    free (f);
	    break;
	}
}

void
cleanup_fade (Display *dpy, win *w)
{
    fade *f = find_fade (w);
    if (f)
	dequeue_fade (dpy, f);
}

void
enqueue_fade (Display *dpy, fade *f)
{
    if (!fades)
	fade_time = get_time_in_milliseconds () + fade_delta;
    f->next = fades;
    fades = f;
}

static void
set_fade (Display *dpy, win *w, Bool in, 
	  void (*callback) (Display *dpy, win *w, Bool gone),
	  Bool gone)
{
    fade    *f;

    f = find_fade (w);
    if (!f)
    {
	f = malloc (sizeof (fade));
	f->next = 0;
	f->w = w;
	if (in)
	    f->cur = 0;
	else
	    f->cur = 1;
	enqueue_fade (dpy, f);
    }
    if (in)
        f->step = fade_step;
    else
	f->step = -fade_step;
    f->callback = callback;
    f->gone = gone;
    w->opacity = f->cur * OPAQUE;
#if 0
    printf ("set_fade start %g step %g\n", f->cur, f->step);
#endif
    determine_mode (dpy, w);
}

int
fade_timeout (void)
{
    int now;
    int	delta;
    if (!fades)
	return -1;
    now = get_time_in_milliseconds();
    delta = fade_time - now;
    if (delta < 0)
	delta = 0;
/*    printf ("timeout %d\n", delta); */
    return delta;
}

void
run_fades (Display *dpy)
{
    int	    now = get_time_in_milliseconds();
    fade    *f, *next;
    int	    steps;

#if 0
    printf ("run fades\n");
#endif
    if (fade_time - now > 0)
	return;
    steps = 1 + (now - fade_time) / fade_delta;
    for (next = fades; f = next; )
    {
	win *w = f->w;
	next = f->next;
	f->cur += f->step * steps;
        if (f->cur >= 1)
	    f->cur = 1;
	else if (f->cur < 0)
	    f->cur = 0;
#if 0
	printf ("opacity now %g\n", f->cur);
#endif
	w->opacity = f->cur * OPAQUE;
	if (f->step > 0)
	{
	    if (f->cur >= 1)
		dequeue_fade (dpy, f);
	}
	else
	{
	    if (f->cur <= 0)
		dequeue_fade (dpy, f);
	}
	determine_mode (dpy, w);
    }
    fade_time = now + fade_delta;
}

#define SHADOW_OPACITY	0.75
#define SHADOW_OFFSET_X	(-shadowRadius * 5 / 4)
#define SHADOW_OFFSET_Y	(-shadowRadius * 5 / 4)

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
    t = 0.0;
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
    
    return ((unsigned char) (v * opacity * 255.0));
}

static XImage *
make_shadow (Display *dpy, double opacity, int width, int height)
{
    XImage	    *ximage;
    unsigned char   *data;
    int		    gsize = gaussianMap->size;
    int		    ylimit, xlimit;
    int		    swidth = width + gsize;
    int		    sheight = height + gsize;
    int		    center = gsize / 2;
    int		    x, y;
    unsigned char   d;
    int		    x_diff;
    
    data = malloc (swidth * sheight * sizeof (unsigned char));
    if (!data)
	return 0;
    ximage = XCreateImage (dpy,
			   DefaultVisual(dpy, DefaultScreen(dpy)),
			   8,
			   ZPixmap,
			   0,
			   (char *) data,
			   swidth, sheight, 8, swidth * sizeof (unsigned char));
    if (!ximage)
    {
	free (data);
	return 0;
    }
    /*
     * Build the gaussian in sections
     */

    /*
     * center (fill the complete data array)
     */

    d = sum_gaussian (gaussianMap, opacity, center, center, width, height);
    memset(data, d, sheight * swidth);
    
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
	    d = sum_gaussian (gaussianMap, opacity, x - center, y - center, width, height);
	    data[y * swidth + x] = d;
	    data[(sheight - y - 1) * swidth + x] = d;
	    data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
	    data[y * swidth + (swidth - x - 1)] = d;
	}

    /*
     * top/bottom
     */
    x_diff = swidth - (gsize * 2);
    if (x_diff > 0 && ylimit > 0)
    {
	for (y = 0; y < ylimit; y++)
	{
	    d = sum_gaussian (gaussianMap, opacity, center, y - center, width, height);
	    memset (&data[y * swidth + gsize], d, x_diff);
	    memset (&data[(sheight - y - 1) * swidth + gsize], d, x_diff);
	}
    }

    /*
     * sides
     */
    
    for (x = 0; x < xlimit; x++)
    {
	d = sum_gaussian (gaussianMap, opacity, x - center, center, width, height);
	for (y = gsize; y < sheight - gsize; y++)
	{
	    data[y * swidth + x] = d;
	    data[y * swidth + (swidth - x - 1)] = d;
	}
    }

    return ximage;
}

static Picture
shadow_picture (Display *dpy, double opacity, int width, int height, int *wp, int *hp)
{
    XImage  *shadowImage;
    Pixmap  shadowPixmap;
    Picture shadowPicture;
    GC	    gc;
    
    shadowImage = make_shadow (dpy, opacity, width, height);
    if (!shadowImage)
	return None;
    shadowPixmap = XCreatePixmap (dpy, root, 
				  shadowImage->width,
				  shadowImage->height,
				  8);
    shadowPicture = XRenderCreatePicture (dpy, shadowPixmap,
					  XRenderFindStandardFormat (dpy, PictStandardA8),
					  0, 0);
    gc = XCreateGC (dpy, shadowPixmap, 0, 0);
    
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

Picture
solid_picture (Display *dpy, Bool argb, double a, double r, double g, double b)
{
    Pixmap			pixmap;
    Picture			picture;
    XRenderPictureAttributes	pa;
    XRenderColor		c;

    pixmap = XCreatePixmap (dpy, root, 1, 1, argb ? 32 : 8);
    pa.repeat = True;
    picture = XRenderCreatePicture (dpy, pixmap,
				    XRenderFindStandardFormat (dpy, argb ? PictStandardARGB32 : PictStandardA8),
				    CPRepeat,
				    &pa);
    c.alpha = a * 0xffff;
    c.red = r * 0xffff;
    c.green = g * 0xffff;
    c.blue = b * 0xffff;
    XRenderFillRectangle (dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
    XFreePixmap (dpy, pixmap);
    return picture;
}

void
discard_ignore (Display *dpy, unsigned long sequence)
{
    while (ignore_head)
    {
	if ((long) (sequence - ignore_head->sequence) > 0)
	{
	    ignore  *next = ignore_head->next;
	    free (ignore_head);
	    ignore_head = next;
	    if (!ignore_head)
		ignore_tail = &ignore_head;
	}
	else
	    break;
    }
}

void
set_ignore (Display *dpy, unsigned long sequence)
{
    ignore  *i = malloc (sizeof (ignore));
    if (!i)
	return;
    i->sequence = sequence;
    i->next = 0;
    *ignore_tail = i;
    ignore_tail = &i->next;
}

int
should_ignore (Display *dpy, unsigned long sequence)
{
    discard_ignore (dpy, sequence);
    return ignore_head && ignore_head->sequence == sequence;
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

static char *backgroundProps[] = {
    "_XROOTPMAP_ID",
    "_XSETROOT_ID",
    0,
};
    
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
    int		    p;

    pixmap = None;
    for (p = 0; backgroundProps[p]; p++)
    {
	if (XGetWindowProperty (dpy, root, XInternAtom (dpy, backgroundProps[p], False),
				0, 4, False, AnyPropertyType,
				&actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success &&
	    actual_type == XInternAtom (dpy, "PIXMAP", False) && actual_format == 32 && nitems == 1)
	{
	    memcpy (&pixmap, prop, 4);
	    XFree (prop);
	    fill = False;
	    break;
	}
    }
    if (!pixmap)
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
    
    r.x = w->a.x;
    r.y = w->a.y;
    r.width = w->a.width + w->a.border_width * 2;
    r.height = w->a.height + w->a.border_width * 2;
    if (compMode != CompSimple)
    {
	if (compMode == CompServerShadows || w->mode != WINDOW_ARGB)
	{
	    XRectangle  sr;

	    if (compMode == CompServerShadows)
	    {
		w->shadow_dx = 2;
		w->shadow_dy = 7;
		w->shadow_width = w->a.width;
		w->shadow_height = w->a.height;
	    }
	    else
	    {
		w->shadow_dx = SHADOW_OFFSET_X;
		w->shadow_dy = SHADOW_OFFSET_Y;
		if (!w->shadow)
		{
		    double	opacity = SHADOW_OPACITY;
		    if (w->mode == WINDOW_TRANS)
			opacity = opacity * TRANS_OPACITY;
		    w->shadow = shadow_picture (dpy, opacity,
						w->a.width + w->a.border_width * 2,
						w->a.height + w->a.border_width * 2,
						&w->shadow_width, &w->shadow_height);
		}
	    }
	    sr.x = w->a.x + w->shadow_dx;
	    sr.y = w->a.y + w->shadow_dy;
	    sr.width = w->shadow_width;
	    sr.height = w->shadow_height;
	    if (sr.x < r.x)
	    {
		r.width = (r.x + r.width) - sr.x;
		r.x = sr.x;
	    }
	    if (sr.y < r.y)
	    {
		r.height = (r.y + r.height) - sr.y;
		r.y = sr.y;
	    }
	    if (sr.x + sr.width > r.x + r.width)
		r.width = sr.x + sr.width - r.x;
	    if (sr.y + sr.height > r.y + r.height)
		r.height = sr.y + sr.height - r.y;
	}
    }
    return XFixesCreateRegion (dpy, &r, 1);
}

static XserverRegion
border_size (Display *dpy, win *w)
{
    XserverRegion   border;
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */
    set_ignore (dpy, NextRequest (dpy));
    border = XFixesCreateRegionFromWindow (dpy, w->id, WindowRegionBounding);
    /* translate this */
    set_ignore (dpy, NextRequest (dpy));
    XFixesTranslateRegion (dpy, border,
			   w->a.x + w->a.border_width,
			   w->a.y + w->a.border_width);
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
#if MONITOR_REPAINT
    rootBuffer = rootPicture;
#else
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
#endif
    XFixesSetPictureClipRegion (dpy, rootPicture, 0, 0, region);
#if MONITOR_REPAINT
    XRenderComposite (dpy, PictOpSrc, blackPicture, None, rootPicture,
		      0, 0, 0, 0, 0, 0, root_width, root_height);
#endif
#if DEBUG_REPAINT
    printf ("paint:");
#endif
    for (w = list; w; w = w->next)
    {
#if CAN_DO_USABLE
	if (!w->usable)
	    continue;
#endif
	/* never painted, ignore it */
	if (!w->damaged)
	    continue;
	if (!w->picture)
	{
	    XRenderPictureAttributes	pa;
	    XRenderPictFormat		*format;
	    Drawable			draw = w->id;
	    
#if HAS_NAME_WINDOW_PIXMAP
	    if (hasNamePixmap && !w->pixmap)
		w->pixmap = XCompositeNameWindowPixmap (dpy, w->id);
	    if (w->pixmap)
		draw = w->pixmap;
#endif
	    format = XRenderFindVisualFormat (dpy, w->a.visual);
	    pa.subwindow_mode = IncludeInferiors;
	    w->picture = XRenderCreatePicture (dpy, draw,
					       format,
					       CPSubwindowMode,
					       &pa);
	}
#if DEBUG_REPAINT
	printf (" 0x%x", w->id);
#endif
	if (clipChanged)
	{
	    if (w->borderSize)
	    {
		set_ignore (dpy, NextRequest (dpy));
		XFixesDestroyRegion (dpy, w->borderSize);
		w->borderSize = None;
	    }
	    if (w->extents)
	    {
		XFixesDestroyRegion (dpy, w->extents);
		w->extents = None;
	    }
	    if (w->borderClip)
	    {
		XFixesDestroyRegion (dpy, w->borderClip);
		w->borderClip = None;
	    }
	}
	if (!w->borderSize)
	    w->borderSize = border_size (dpy, w);
	if (!w->extents)
	    w->extents = win_extents (dpy, w);
	if (w->mode == WINDOW_SOLID)
	{
	    int	x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
	    x = w->a.x;
	    y = w->a.y;
	    wid = w->a.width + w->a.border_width * 2;
	    hei = w->a.height + w->a.border_width * 2;
#else
	    x = w->a.x + w->a.border_width;
	    y = w->a.y + w->a.border_width;
	    wid = w->a.width;
	    hei = w->a.height;
#endif
	    XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, region);
	    set_ignore (dpy, NextRequest (dpy));
	    XFixesSubtractRegion (dpy, region, region, w->borderSize);
	    set_ignore (dpy, NextRequest (dpy));
	    XRenderComposite (dpy, PictOpSrc, w->picture, None, rootBuffer,
			      0, 0, 0, 0, 
			      x, y, wid, hei);
	}
	if (!w->borderClip)
	{
	    w->borderClip = XFixesCreateRegion (dpy, 0, 0);
	    XFixesCopyRegion (dpy, w->borderClip, region);
	}
	w->prev_trans = t;
	t = w;
    }
#if DEBUG_REPAINT
    printf ("\n");
    fflush (stdout);
#endif
    XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, region);
    paint_root (dpy);
    for (w = t; w; w = w->prev_trans)
    {
	XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, w->borderClip);
	switch (compMode) {
	case CompSimple:
	    break;
	case CompServerShadows:
	    set_ignore (dpy, NextRequest (dpy));
	    if (w->opacity != OPAQUE && !w->shadowPict)
		w->shadowPict = solid_picture (dpy, True,
					       (double) w->opacity / OPAQUE * 0.3,
					       0, 0, 0);
	    XRenderComposite (dpy, PictOpOver, 
			      w->shadowPict ? w->shadowPict : transBlackPicture,
			      w->picture, rootBuffer,
			      0, 0, 0, 0,
			      w->a.x + w->shadow_dx,
			      w->a.y + w->shadow_dy,
			      w->shadow_width, w->shadow_height);
	    break;
	case CompClientShadows:
	    if (w->shadow)
	    {
		XRenderComposite (dpy, PictOpOver, blackPicture, w->shadow, rootBuffer,
				  0, 0, 0, 0,
				  w->a.x + w->shadow_dx,
				  w->a.y + w->shadow_dy,
				  w->shadow_width, w->shadow_height);
	    }
	    break;
	}
	if (w->opacity != OPAQUE && !w->alphaPict)
	    w->alphaPict = solid_picture (dpy, False, 
					  (double) w->opacity / OPAQUE, 0, 0, 0);
	if (w->mode == WINDOW_TRANS)
	{
	    int	x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
	    x = w->a.x;
	    y = w->a.y;
	    wid = w->a.width + w->a.border_width * 2;
	    hei = w->a.height + w->a.border_width * 2;
#else
	    x = w->a.x + w->a.border_width;
	    y = w->a.y + w->a.border_width;
	    wid = w->a.width;
	    hei = w->a.height;
#endif
	    set_ignore (dpy, NextRequest (dpy));
	    XRenderComposite (dpy, PictOpOver, w->picture, w->alphaPict, rootBuffer,
			      0, 0, 0, 0, 
			      x, y, wid, hei);
	}
	else if (w->mode == WINDOW_ARGB)
	{
	    int	x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
	    x = w->a.x;
	    y = w->a.y;
	    wid = w->a.width + w->a.border_width * 2;
	    hei = w->a.height + w->a.border_width * 2;
#else
	    x = w->a.x + w->a.border_width;
	    y = w->a.y + w->a.border_width;
	    wid = w->a.width;
	    hei = w->a.height;
#endif
	    set_ignore (dpy, NextRequest (dpy));
	    XRenderComposite (dpy, PictOpOver, w->picture, w->alphaPict, rootBuffer,
			      0, 0, 0, 0, 
			      x, y, wid, hei);
	}
	XFixesDestroyRegion (dpy, w->borderClip);
	w->borderClip = None;
    }
    XFixesDestroyRegion (dpy, region);
    if (rootBuffer != rootPicture)
    {
	XFixesSetPictureClipRegion (dpy, rootBuffer, 0, 0, None);
	XRenderComposite (dpy, PictOpSrc, rootBuffer, None, rootPicture,
			  0, 0, 0, 0, 0, 0, root_width, root_height);
    }
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
repair_win (Display *dpy, win *w)
{
    XserverRegion   parts;

    if (!w->damaged)
    {
	parts = win_extents (dpy, w);
	set_ignore (dpy, NextRequest (dpy));
	XDamageSubtract (dpy, w->damage, None, None);
    }
    else
    {
	XserverRegion	o;
	parts = XFixesCreateRegion (dpy, 0, 0);
	set_ignore (dpy, NextRequest (dpy));
	XDamageSubtract (dpy, w->damage, None, parts);
	XFixesTranslateRegion (dpy, parts,
			       w->a.x + w->a.border_width,
			       w->a.y + w->a.border_width);
	if (compMode == CompServerShadows)
	{
	    o = XFixesCreateRegion (dpy, 0, 0);
	    XFixesCopyRegion (dpy, o, parts);
	    XFixesTranslateRegion (dpy, o, w->shadow_dx, w->shadow_dy);
	    XFixesUnionRegion (dpy, parts, parts, o);
	    XFixesDestroyRegion (dpy, o);
	}
    }
    add_damage (dpy, parts);
    w->damaged = 1;
}

static void
map_win (Display *dpy, Window id, unsigned long sequence, Bool fade)
{
    win		*w = find_win (dpy, id);
    Drawable	back;

    if (!w)
	return;
    w->a.map_state = IsViewable;
    
#if CAN_DO_USABLE
    w->damage_bounds.x = w->damage_bounds.y = 0;
    w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
    w->damaged = 0;
}

static void
finish_unmap_win (Display *dpy, win *w)
{
    w->damaged = 0;
#if CAN_DO_USABLE
    w->usable = False;
#endif
    if (w->extents != None)
    {
	add_damage (dpy, w->extents);    /* destroys region */
	w->extents = None;
    }
    
#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap)
    {
	XFreePixmap (dpy, w->pixmap);
	w->pixmap = None;
    }
#endif

    if (w->picture)
    {
	set_ignore (dpy, NextRequest (dpy));
	XRenderFreePicture (dpy, w->picture);
	w->picture = None;
    }

    /* don't care about properties anymore */
    set_ignore (dpy, NextRequest (dpy));
    XSelectInput(dpy, w->id, 0);

    if (w->borderSize)
    {
	set_ignore (dpy, NextRequest (dpy));
    	XFixesDestroyRegion (dpy, w->borderSize);
    	w->borderSize = None;
    }
    if (w->shadow)
    {
	XRenderFreePicture (dpy, w->shadow);
	w->shadow = None;
    }
    if (w->borderClip)
    {
	XFixesDestroyRegion (dpy, w->borderClip);
	w->borderClip = None;
    }

    clipChanged = True;
}

#if HAS_NAME_WINDOW_PIXMAP
static void
unmap_callback (Display *dpy, win *w, Bool gone)
{
    finish_unmap_win (dpy, w);
}
#endif

static void
unmap_win (Display *dpy, Window id, Bool fade)
{
    win *w = find_win (dpy, id);
    if (!w)
	return;
#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap && fade && fadeWindows)
	set_fade (dpy, w, False, unmap_callback, False);
    else
#endif
	finish_unmap_win (dpy, w);
}

/* Get the opacity prop from window
   not found: default
   otherwise the value
 */
static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def)
{
    Atom actual;
    int format;
    unsigned long n, left;

    char *data;
    XGetWindowProperty(dpy, w->id, opacityAtom, 0L, 1L, False, 
		       XA_CARDINAL, &actual, &format, 
		       &n, &left, (unsigned char **) &data);
    if (data != None)
    {
	unsigned int i;
	memcpy (&i, data, sizeof (unsigned int));
	XFree( (void *) data);
	return i;
    }
    return def;
}

/* determine mode for window all in one place.
   Future might check for menu flag and other cool things
*/

static void
determine_mode(Display *dpy, win *w)
{
    int mode;
    XRenderPictFormat *format;
    unsigned int default_opacity;

    /* if trans prop == -1 fall back on  previous tests*/

    if (w->alphaPict)
    {
	XRenderFreePicture (dpy, w->alphaPict);
	w->alphaPict = None;
    }
    if (w->shadowPict)
    {
	XRenderFreePicture (dpy, w->shadowPict);
	w->shadowPict = None;
    }

    if (w->a.class == InputOnly)
    {
	format = 0;
    }
    else
    {
	format = XRenderFindVisualFormat (dpy, w->a.visual);
    }

    if (format && format->type == PictTypeDirect && format->direct.alphaMask)
    {
	mode = WINDOW_ARGB;
    }
    else if (w->opacity != OPAQUE)
    {
	mode = WINDOW_TRANS;
    }
    else
    {
	mode = WINDOW_SOLID;
    }
    w->mode = mode;
    if (w->extents)
    {
	XserverRegion damage;
	damage = XFixesCreateRegion (dpy, 0, 0);
	XFixesCopyRegion (dpy, damage, w->extents);
	add_damage (dpy, damage);
    }
}

static void
add_win (Display *dpy, Window id, Window prev)
{
    win				*new = malloc (sizeof (win));
    win				**p;
    
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
    set_ignore (dpy, NextRequest (dpy));
    if (!XGetWindowAttributes (dpy, id, &new->a))
    {
	free (new);
	return;
    }
    new->damaged = 0;
#if CAN_DO_USABLE
    new->usable = False;
#endif
#if HAS_NAME_WINDOW_PIXMAP
    new->pixmap = None;
#endif
    new->picture = None;
    if (new->a.class == InputOnly)
    {
	new->damage_sequence = 0;
	new->damage = None;
    }
    else
    {
	new->damage_sequence = NextRequest (dpy);
	new->damage = XDamageCreate (dpy, id, XDamageReportNonEmpty);
    }
    new->alphaPict = None;
    new->shadowPict = None;
    new->borderSize = None;
    new->extents = None;
    new->shadow = None;
    new->shadow_dx = 0;
    new->shadow_dy = 0;
    new->shadow_width = 0;
    new->shadow_height = 0;
    new->opacity = OPAQUE;

    new->borderClip = None;
    new->prev_trans = 0;

    /* moved mode setting to one place */
    XSelectInput(dpy, id, PropertyChangeMask);
    new->opacity = get_opacity_prop(dpy, new, OPAQUE);
    determine_mode (dpy, new);
    
    new->next = *p;
    *p = new;
    if (new->a.map_state == IsViewable)
	map_win (dpy, id, new->damage_sequence - 1, False);
}

void
restack_win (Display *dpy, win *w, Window new_above)
{
    Window  old_above;
    
    if (w->next)
	old_above = w->next->id;
    else
	old_above = None;
    if (old_above != new_above)
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
	    if ((*prev)->id == new_above)
		break;
	}
	w->next = *prev;
	*prev = w;
    }
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
#if CAN_DO_USABLE
    if (w->usable)
#endif
    {
	damage = XFixesCreateRegion (dpy, 0, 0);
	if (w->extents != None)	
	    XFixesCopyRegion (dpy, damage, w->extents);
    }
    w->a.x = ce->x;
    w->a.y = ce->y;
    if (w->a.width != ce->width || w->a.height != ce->height)
    {
#if HAS_NAME_WINDOW_PIXMAP
	if (w->pixmap)
	{
	    XFreePixmap (dpy, w->pixmap);
	    w->pixmap = None;
	    if (w->picture)
	    {
		XRenderFreePicture (dpy, w->picture);
		w->picture = None;
	    }
	}
#endif
	if (w->shadow)
	{
	    XRenderFreePicture (dpy, w->shadow);
	    w->shadow = None;
	}
    }
    w->a.width = ce->width;
    w->a.height = ce->height;
    w->a.border_width = ce->border_width;
    w->a.override_redirect = ce->override_redirect;
    restack_win (dpy, w, ce->above);
    if (damage)
    {
	XserverRegion	extents = win_extents (dpy, w);
	XFixesUnionRegion (dpy, damage, damage, extents);
	XFixesDestroyRegion (dpy, extents);
	add_damage (dpy, damage);
    }
    clipChanged = True;
}

static void
circulate_win (Display *dpy, XCirculateEvent *ce)
{
    win	    *w = find_win (dpy, ce->window);
    Window  new_above;

    if (ce->place == PlaceOnTop)
	new_above = list->id;
    else
	new_above = None;
    restack_win (dpy, w, new_above);
    clipChanged = True;
}

static void
finish_destroy_win (Display *dpy, Window id, Bool gone)
{
    win	**prev, *w;

    for (prev = &list; (w = *prev); prev = &w->next)
	if (w->id == id)
	{
	    if (!gone)
		finish_unmap_win (dpy, w);
	    *prev = w->next;
	    if (w->picture)
	    {
		set_ignore (dpy, NextRequest (dpy));
		XRenderFreePicture (dpy, w->picture);
	    }
	    if (w->alphaPict)
	    {
		XRenderFreePicture (dpy, w->alphaPict);
		w->alphaPict = None;
	    }
	    if (w->shadowPict)
	    {
		XRenderFreePicture (dpy, w->shadowPict);
		w->shadowPict = None;
	    }
	    if (w->damage != None)
	    {
		set_ignore (dpy, NextRequest (dpy));
		XDamageDestroy (dpy, w->damage);
	    }
	    cleanup_fade (dpy, w);
	    free (w);
	    break;
	}
}

#if HAS_NAME_WINDOW_PIXMAP
static void
destroy_callback (Display *dpy, win *w, Bool gone)
{
    finish_destroy_win (dpy, w->id, gone);
}
#endif

static void
destroy_win (Display *dpy, Window id, Bool gone, Bool fade)
{
    win *w = find_win (dpy, id);
#if HAS_NAME_WINDOW_PIXMAP
    if (w && w->pixmap && fade && fadeWindows)
	set_fade (dpy, w, False, destroy_callback, gone);
    else
#endif
    {
	finish_destroy_win (dpy, id, gone);
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
    win	*w = find_win (dpy, de->drawable);

    if (!w)
	return;
#if CAN_DO_USABLE
    if (!w->usable)
    {
	if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0)
	{
	    w->damage_bounds = de->area;
	}
	else
	{
	    if (de->area.x < w->damage_bounds.x)
	    {
		w->damage_bounds.width += (w->damage_bounds.x - de->area.x);
		w->damage_bounds.x = de->area.x;
	    }
	    if (de->area.y < w->damage_bounds.y)
	    {
		w->damage_bounds.height += (w->damage_bounds.y - de->area.y);
		w->damage_bounds.y = de->area.y;
	    }
	    if (de->area.x + de->area.width > w->damage_bounds.x + w->damage_bounds.width)
		w->damage_bounds.width = de->area.x + de->area.width - w->damage_bounds.x;
	    if (de->area.y + de->area.height > w->damage_bounds.y + w->damage_bounds.height)
		w->damage_bounds.height = de->area.y + de->area.height - w->damage_bounds.y;
	}
#if 0
	printf ("unusable damage %d, %d: %d x %d bounds %d, %d: %d x %d\n",
		de->area.x,
		de->area.y,
		de->area.width,
		de->area.height,
		w->damage_bounds.x,
		w->damage_bounds.y,
		w->damage_bounds.width,
		w->damage_bounds.height);
#endif
	if (w->damage_bounds.x <= 0 && 
	    w->damage_bounds.y <= 0 &&
	    w->a.width <= w->damage_bounds.x + w->damage_bounds.width &&
	    w->a.height <= w->damage_bounds.y + w->damage_bounds.height)
	{
	    clipChanged = True;
	    if (fadeWindows)
		set_fade (dpy, w, True, 0, False);
	    w->usable = True;
	}
    }
    if (w->usable)
#endif
	repair_win (dpy, w);
}

static int
error (Display *dpy, XErrorEvent *ev)
{
    int	    o;
    char    *name = 0;
    
    if (should_ignore (dpy, ev->serial))
	return 0;
    
    if (ev->request_code == composite_opcode &&
	ev->minor_code == X_CompositeRedirectSubwindows)
    {
	fprintf (stderr, "Another composite manager is already running\n");
	exit (1);
    }
    
    o = ev->error_code - xfixes_error;
    switch (o) {
    case BadRegion: name = "BadRegion";	break;
    default: break;
    }
    o = ev->error_code - damage_error;
    switch (o) {
    case BadDamage: name = "BadDamage";	break;
    default: break;
    }
    o = ev->error_code - render_error;
    switch (o) {
    case BadPictFormat: name ="BadPictFormat"; break;
    case BadPicture: name ="BadPicture"; break;
    case BadPictOp: name ="BadPictOp"; break;
    case BadGlyphSet: name ="BadGlyphSet"; break;
    case BadGlyph: name ="BadGlyph"; break;
    default: break;
    }
	
    printf ("error %d request %d minor %d serial %d\n",
	    ev->error_code, ev->request_code, ev->minor_code, ev->serial);

/*    abort ();	    this is just annoying to most people */
    return 0;
}

static void
expose_root (Display *dpy, Window root, XRectangle *rects, int nrects)
{
    XserverRegion  region = XFixesCreateRegion (dpy, rects, nrects);
    
    add_damage (dpy, region);
}


static int
ev_serial (XEvent *ev)
{
    if (ev->type & 0x7f != KeymapNotify)
	return ev->xany.serial;
    return NextRequest (ev->xany.display);
}


static char *
ev_name (XEvent *ev)
{
    static char	buf[128];
    switch (ev->type & 0x7f) {
    case Expose:
	return "Expose";
    case MapNotify:
	return "Map";
    case UnmapNotify:
	return "Unmap";
    case ReparentNotify:
	return "Reparent";
    case CirculateNotify:
	return "Circulate";
    default:
    	if (ev->type == damage_event + XDamageNotify)
	    return "Damage";
	sprintf (buf, "Event %d", ev->type);
	return buf;
    }
}

static Window
ev_window (XEvent *ev)
{
    switch (ev->type) {
    case Expose:
	return ev->xexpose.window;
    case MapNotify:
	return ev->xmap.window;
    case UnmapNotify:
	return ev->xunmap.window;
    case ReparentNotify:
	return ev->xreparent.window;
    case CirculateNotify:
	return ev->xcirculate.window;
    default:
    	if (ev->type == damage_event + XDamageNotify)
	    return ((XDamageNotifyEvent *) ev)->drawable;
	return 0;
    }
}

void
usage (char *program)
{
    fprintf (stderr, "usage: %s [-d display] [-n] [-s] [-c] [-a]\n", program);
    exit (1);
}

int
main (int argc, char **argv)
{
    XEvent	    ev;
    Window	    root_return, parent_return;
    Window	    *children;
    Pixmap	    transPixmap;
    Pixmap	    blackPixmap;
    unsigned int    nchildren;
    int		    i;
    XRenderPictureAttributes	pa;
    XRenderColor		c;
    XRectangle	    *expose_rects = 0;
    int		    size_expose = 0;
    int		    n_expose = 0;
    struct pollfd   ufd;
    int		    n;
    int		    last_update;
    int		    now;
    int		    p;
    int		    composite_major, composite_minor;
    char	    *display = 0;
    int		    o;

    while ((o = getopt (argc, argv, "d:scnfaS")) != -1)
    {
	switch (o) {
	case 'd':
	    display = optarg;
	    break;
	case 's':
	    compMode = CompServerShadows;
	    break;
	case 'c':
	    compMode = CompClientShadows;
	    break;
	case 'n':
	    compMode = CompSimple;
	    break;
	case 'f':
	    fadeWindows = True;
	    break;
	case 'a':
	    autoRedirect = True;
	    break;
	case 'S':
	    synchronize = True;
	    break;
	default:
	    usage (argv[0]);
	    break;
	}
    }
    
    dpy = XOpenDisplay (display);
    if (!dpy)
    {
	fprintf (stderr, "Can't open display\n");
	exit (1);
    }
    XSetErrorHandler (error);
    if (synchronize)
	XSynchronize (dpy, 1);
    scr = DefaultScreen (dpy);
    root = RootWindow (dpy, scr);

    if (!XRenderQueryExtension (dpy, &render_event, &render_error))
    {
	fprintf (stderr, "No render extension\n");
	exit (1);
    }
    if (!XQueryExtension (dpy, COMPOSITE_NAME, &composite_opcode,
			  &composite_event, &composite_error))
    {
	fprintf (stderr, "No composite extension\n");
	exit (1);
    }
    XCompositeQueryVersion (dpy, &composite_major, &composite_minor);
#if HAS_NAME_WINDOW_PIXMAP
    if (composite_major > 0 || composite_minor >= 2)
	hasNamePixmap = True;
#endif

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
    /* get atoms */
    opacityAtom = XInternAtom (dpy, OPACITY_PROP, False);

    pa.subwindow_mode = IncludeInferiors;

    if (compMode == CompClientShadows)
	gaussianMap = make_gaussian_map(dpy, shadowRadius);

    root_width = DisplayWidth (dpy, scr);
    root_height = DisplayHeight (dpy, scr);

    rootPicture = XRenderCreatePicture (dpy, root, 
					XRenderFindVisualFormat (dpy,
								 DefaultVisual (dpy, scr)),
					CPSubwindowMode,
					&pa);
    blackPicture = solid_picture (dpy, True, 1, 0, 0, 0);
    if (compMode == CompServerShadows)
	transBlackPicture = solid_picture (dpy, True, 0.3, 0, 0, 0);
    allDamage = None;
    clipChanged = True;
    XGrabServer (dpy);
    if (autoRedirect)
	XCompositeRedirectSubwindows (dpy, root, CompositeRedirectAutomatic);
    else
    {
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
    }
    XUngrabServer (dpy);
    ufd.fd = ConnectionNumber (dpy);
    ufd.events = POLLIN;
    if (!autoRedirect)
	paint_all (dpy, None);
    for (;;)
    {
	/*	dump_wins (); */
	do {
	    if (autoRedirect)
		XFlush (dpy);
            if (!QLength (dpy))
            {
        	 if (poll (&ufd, 1, fade_timeout()) == 0)
		 {
		    run_fades (dpy);
		    break;
		 }
	    }

	    XNextEvent (dpy, &ev);
	    if (ev.type & 0x7f != KeymapNotify)
		discard_ignore (dpy, ev.xany.serial);
#if DEBUG_EVENTS
	    printf ("event %10.10s serial 0x%08x window 0x%08x\n",
		    ev_name(&ev), ev_serial (&ev), ev_window (&ev));
#endif
	    if (!autoRedirect) switch (ev.type) {
	    case CreateNotify:
		add_win (dpy, ev.xcreatewindow.window, 0);
		break;
	    case ConfigureNotify:
		configure_win (dpy, &ev.xconfigure);
		break;
	    case DestroyNotify:
		destroy_win (dpy, ev.xdestroywindow.window, True, True);
		break;
	    case MapNotify:
		map_win (dpy, ev.xmap.window, ev.xmap.serial, True);
		break;
	    case UnmapNotify:
		unmap_win (dpy, ev.xunmap.window, True);
		break;
	    case ReparentNotify:
		if (ev.xreparent.parent == root)
		    add_win (dpy, ev.xreparent.window, 0);
		else
		    destroy_win (dpy, ev.xreparent.window, False, True);
		break;
	    case CirculateNotify:
		circulate_win (dpy, &ev.xcirculate);
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
		for (p = 0; backgroundProps[p]; p++)
		{
		    if (ev.xproperty.atom == XInternAtom (dpy, backgroundProps[p], False))
		    {
			if (rootTile)
			{
			    XClearArea (dpy, root, 0, 0, 0, 0, True);
			    XRenderFreePicture (dpy, rootTile);
			    rootTile = None;
			    break;
			}
		    }
		}
		/* check if Trans property was changed */
		if (ev.xproperty.atom == opacityAtom)
		{
		    /* reset mode and redraw window */
		    win * w = find_win(dpy, ev.xproperty.window);
		    if (w)
		    {
			w->opacity = get_opacity_prop(dpy, w, OPAQUE);
			determine_mode(dpy, w);
		    }
		}
		break;
	    default:
		if (ev.type == damage_event + XDamageNotify)
		    damage_win (dpy, (XDamageNotifyEvent *) &ev);
		break;
	    }
	} while (QLength (dpy));
	if (allDamage && !autoRedirect)
	{
	    static int	paint;
	    paint_all (dpy, allDamage);
	    paint++;
	    XSync (dpy, False);
	    allDamage = None;
	    clipChanged = False;
	}
    }
}
