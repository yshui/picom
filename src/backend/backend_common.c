#include <xcb/xcb_image.h>

#include "render.h"
#include "backend_common.h"

/**
 * Generate a 1x1 <code>Picture</code> of a particular color.
 */
xcb_render_picture_t
solid_picture(session_t *ps, bool argb, double a, double r, double g, double b) {
	xcb_pixmap_t pixmap;
	xcb_render_picture_t picture;
	xcb_render_create_picture_value_list_t pa;
	xcb_render_color_t col;
	xcb_rectangle_t rect;

	pixmap = x_create_pixmap(ps, argb ? 32 : 8, ps->root, 1, 1);
	if (!pixmap)
		return None;

	pa.repeat = True;
	picture = x_create_picture_with_standard_and_pixmap(
	    ps, argb ? XCB_PICT_STANDARD_ARGB_32 : XCB_PICT_STANDARD_A_8, pixmap,
	    XCB_RENDER_CP_REPEAT, &pa);

	if (!picture) {
		xcb_free_pixmap(ps->c, pixmap);
		return None;
	}

	col.alpha = a * 0xffff;
	col.red = r * 0xffff;
	col.green = g * 0xffff;
	col.blue = b * 0xffff;

	rect.x = 0;
	rect.y = 0;
	rect.width = 1;
	rect.height = 1;

	xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC, picture, col, 1, &rect);
	xcb_free_pixmap(ps->c, pixmap);

	return picture;
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
bool build_shadow(session_t *ps, double opacity, const int width, const int height,
                  xcb_render_picture_t shadow_pixel, xcb_pixmap_t *pixmap,
                  xcb_render_picture_t *pict) {
	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = None, shadow_pixmap_argb = None;
	xcb_render_picture_t shadow_picture = None, shadow_picture_argb = None;
	xcb_gcontext_t gc = None;

	shadow_image = make_shadow(ps, opacity, width, height);
	if (!shadow_image) {
		log_error("Failed to make shadow");
		return false;
	}

	shadow_pixmap =
	    x_create_pixmap(ps, 8, ps->root, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(ps, 32, ps->root, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("Failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb)
		goto shadow_picture_err;

	gc = xcb_generate_id(ps->c);
	xcb_create_gc(ps->c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(ps->c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, shadow_pixel, shadow_picture,
	                     shadow_picture_argb, 0, 0, 0, 0, 0, 0, shadow_image->width,
	                     shadow_image->height);

	*pixmap = shadow_pixmap_argb;
	*pict = shadow_picture_argb;

	xcb_free_gc(ps->c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(ps->c, shadow_pixmap);
	xcb_render_free_picture(ps->c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image)
		xcb_image_destroy(shadow_image);
	if (shadow_pixmap)
		xcb_free_pixmap(ps->c, shadow_pixmap);
	if (shadow_pixmap_argb)
		xcb_free_pixmap(ps->c, shadow_pixmap_argb);
	if (shadow_picture)
		xcb_render_free_picture(ps->c, shadow_picture);
	if (shadow_picture_argb)
		xcb_render_free_picture(ps->c, shadow_picture_argb);
	if (gc)
		xcb_free_gc(ps->c, gc);

	return false;
}

// vim: set noet sw=8 ts=8 :
