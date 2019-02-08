// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

/// Function pointers to init VSync modes.

#include <GL/glx.h>

#include "common.h"
#include "log.h"

#ifdef CONFIG_OPENGL
#include "backend/gl/glx.h"
#include "opengl.h"
#endif

#ifdef CONFIG_VSYNC_DRM
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <drm.h>
#include <errno.h>
#include <sys/ioctl.h>
#endif

#include "config.h"
#include "vsync.h"

#ifdef CONFIG_VSYNC_DRM
/**
 * Wait for next VSync, DRM method.
 *
 * Stolen from: https://github.com/MythTV/mythtv/blob/master/mythtv/libs/libmythtv/vsync.cpp
 */
static int
vsync_drm_wait(session_t *ps) {
  int ret = -1;
  drm_wait_vblank_t vbl;

  vbl.request.type = _DRM_VBLANK_RELATIVE,
  vbl.request.sequence = 1;

  do {
     ret = ioctl(ps->drm_fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
     vbl.request.type &= ~_DRM_VBLANK_RELATIVE;
  } while (ret && errno == EINTR);

  if (ret)
    log_error("VBlank ioctl did not work, unimplemented in this drmver?");

  return ret;

}
#endif

/**
 * Initialize DRM VSync.
 *
 * @return true for success, false otherwise
 */
static bool
vsync_drm_init(session_t *ps) {
#ifdef CONFIG_VSYNC_DRM
  // Should we always open card0?
  if (ps->drm_fd < 0 && (ps->drm_fd = open("/dev/dri/card0", O_RDWR)) < 0) {
    log_error("Failed to open device.");
    return false;
  }

  if (vsync_drm_wait(ps))
    return false;

  return true;
#else
  log_error("compton is not compiled with DRM VSync support.");
  return false;
#endif
}

/**
 * Initialize OpenGL VSync.
 *
 * Stolen from: http://git.tuxfamily.org/?p=ccm/cairocompmgr.git;a=commitdiff;h=efa4ceb97da501e8630ca7f12c99b1dce853c73e
 * Possible original source: http://www.inb.uni-luebeck.de/~boehme/xvideo_sync.html
 *
 * @return true for success, false otherwise
 */
static bool
vsync_opengl_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!ensure_glx_context(ps))
    return false;

  if (!glx_has_extension(ps->dpy, ps->scr, "GLX_SGI_video_sync")) {
    log_error("Your driver doesn't support SGI_video_sync, giving up.");
    return false;
  }

  // Get video sync functions
  if (!ps->psglx->glXGetVideoSyncSGI)
    ps->psglx->glXGetVideoSyncSGI = (f_GetVideoSync)
      glXGetProcAddress((const GLubyte *) "glXGetVideoSyncSGI");
  if (!ps->psglx->glXWaitVideoSyncSGI)
    ps->psglx->glXWaitVideoSyncSGI = (f_WaitVideoSync)
      glXGetProcAddress((const GLubyte *) "glXWaitVideoSyncSGI");
  if (!ps->psglx->glXWaitVideoSyncSGI || !ps->psglx->glXGetVideoSyncSGI) {
    log_error("Failed to get glXWait/GetVideoSyncSGI function.");
    return false;
  }

  return true;
#else
  log_error("compton is not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_oml_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!ensure_glx_context(ps))
    return false;

  if (!glx_has_extension(ps->dpy, ps->scr, "GLX_OML_sync_control")) {
    log_error("Your driver doesn't support OML_sync_control, giving up.");
    return false;
  }

  // Get video sync functions
  if (!ps->psglx->glXGetSyncValuesOML)
    ps->psglx->glXGetSyncValuesOML = (f_GetSyncValuesOML)
      glXGetProcAddress ((const GLubyte *) "glXGetSyncValuesOML");
  if (!ps->psglx->glXWaitForMscOML)
    ps->psglx->glXWaitForMscOML = (f_WaitForMscOML)
      glXGetProcAddress ((const GLubyte *) "glXWaitForMscOML");
  if (!ps->psglx->glXGetSyncValuesOML || !ps->psglx->glXWaitForMscOML) {
    log_error("Failed to get OML_sync_control functions.");
    return false;
  }

  return true;
#else
  log_error("compton is not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_swc_swap_interval(session_t *ps, unsigned int interval) {
#ifdef CONFIG_OPENGL
  if (!ensure_glx_context(ps))
    return false;

  if (!ps->psglx->glXSwapIntervalProc && !ps->psglx->glXSwapIntervalMESAProc) {
    if (glx_has_extension(ps->dpy, ps->scr, "GLX_MESA_swap_control")) {
      ps->psglx->glXSwapIntervalMESAProc = (f_SwapIntervalMESA)
        glXGetProcAddress ((const GLubyte *) "glXSwapIntervalMESA");
    } else if (glx_has_extension(ps->dpy, ps->scr, "GLX_SGI_swap_control")) {
      ps->psglx->glXSwapIntervalProc = (f_SwapIntervalSGI)
        glXGetProcAddress ((const GLubyte *) "glXSwapIntervalSGI");
    } else {
      log_error("Your driver doesn't support SGI_swap_control nor MESA_swap_control, giving up.");
      return false;
    }
  }

  if (ps->psglx->glXSwapIntervalMESAProc)
    ps->psglx->glXSwapIntervalMESAProc(interval);
  else if (ps->psglx->glXSwapIntervalProc)
    ps->psglx->glXSwapIntervalProc(interval);
  else
    return false;
#endif

  return true;
}

static bool
vsync_opengl_swc_init(session_t *ps) {
#ifdef CONFIG_OPENGL
  if (!bkend_use_glx(ps)) {
    log_warn("OpenGL swap control requires the GLX backend.");
    return false;
  }

  if (!vsync_opengl_swc_swap_interval(ps, 1)) {
    log_error("Failed to load a swap control extension.");
    return false;
  }

  return true;
#else
  log_error("compton is not compiled with OpenGL VSync support.");
  return false;
#endif
}

static bool
vsync_opengl_mswc_init(session_t *ps) {
  log_warn("opengl-mswc is deprecated, please use opengl-swc instead.");
  return vsync_opengl_swc_init(ps);
}

bool (*const VSYNC_FUNCS_INIT[NUM_VSYNC])(session_t *ps) = {
  [VSYNC_DRM          ] = vsync_drm_init,
  [VSYNC_OPENGL       ] = vsync_opengl_init,
  [VSYNC_OPENGL_OML   ] = vsync_opengl_oml_init,
  [VSYNC_OPENGL_SWC   ] = vsync_opengl_swc_init,
  [VSYNC_OPENGL_MSWC  ] = vsync_opengl_mswc_init,
};

#ifdef CONFIG_OPENGL
/**
 * Wait for next VSync, OpenGL method.
 */
static int
vsync_opengl_wait(session_t *ps) {
  unsigned vblank_count = 0;

  ps->psglx->glXGetVideoSyncSGI(&vblank_count);
  ps->psglx->glXWaitVideoSyncSGI(2, (vblank_count + 1) % 2, &vblank_count);
  // I see some code calling glXSwapIntervalSGI(1) afterwards, is it required?

  return 0;
}

/**
 * Wait for next VSync, OpenGL OML method.
 *
 * https://mail.gnome.org/archives/clutter-list/2012-November/msg00031.html
 */
static int
vsync_opengl_oml_wait(session_t *ps) {
  int64_t ust = 0, msc = 0, sbc = 0;

  ps->psglx->glXGetSyncValuesOML(ps->dpy, ps->reg_win, &ust, &msc, &sbc);
  ps->psglx->glXWaitForMscOML(ps->dpy, ps->reg_win, 0, 2, (msc + 1) % 2,
      &ust, &msc, &sbc);

  return 0;
}

#endif

/// Function pointers to wait for VSync.
int (*const VSYNC_FUNCS_WAIT[NUM_VSYNC])(session_t *ps) = {
#ifdef CONFIG_VSYNC_DRM
  [VSYNC_DRM        ] = vsync_drm_wait,
#endif
#ifdef CONFIG_OPENGL
  [VSYNC_OPENGL     ] = vsync_opengl_wait,
  [VSYNC_OPENGL_OML ] = vsync_opengl_oml_wait,
#endif
};

#ifdef CONFIG_OPENGL
static void
vsync_opengl_swc_deinit(session_t *ps) {
  vsync_opengl_swc_swap_interval(ps, 0);
}
#endif


/// Function pointers to deinitialize VSync.
void (*const VSYNC_FUNCS_DEINIT[NUM_VSYNC])(session_t *ps) = {
#ifdef CONFIG_OPENGL
  [VSYNC_OPENGL_SWC   ] = vsync_opengl_swc_deinit,
  [VSYNC_OPENGL_MSWC  ] = vsync_opengl_swc_deinit,
#endif
};

/**
 * Initialize current VSync method.
 */
bool vsync_init(session_t *ps) {
  // Mesa turns on swap control by default, undo that
  if (bkend_use_glx(ps))
    vsync_opengl_swc_swap_interval(ps, 0);

  if (ps->o.vsync && VSYNC_FUNCS_INIT[ps->o.vsync]
      && !VSYNC_FUNCS_INIT[ps->o.vsync](ps)) {
    ps->o.vsync = VSYNC_NONE;
    return false;
  }
  else
    return true;
}

/**
 * Wait for next VSync.
 */
void vsync_wait(session_t *ps) {
  if (!ps->o.vsync)
    return;

  if (VSYNC_FUNCS_WAIT[ps->o.vsync])
    VSYNC_FUNCS_WAIT[ps->o.vsync](ps);
}

/**
 * Deinitialize current VSync method.
 */
void vsync_deinit(session_t *ps) {
  if (ps->o.vsync && VSYNC_FUNCS_DEINIT[ps->o.vsync])
    VSYNC_FUNCS_DEINIT[ps->o.vsync](ps);
}
