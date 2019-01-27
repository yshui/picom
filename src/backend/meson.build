# enable xrender
srcs += [ files('backend_common.c') ]
if get_option('new_backends')
  srcs += [ files('xrender.c', 'backend.c') ]

  # enable opengl
  if get_option('opengl')
    srcs += [ files('gl/gl_common.c', 'gl/glx.c') ]
  endif
endif

if get_option('opengl')
  srcs += [ files('gl/gl_common.c') ]
endif
