# enable xrender
srcs += [ files('backend_common.c') ]
if get_option('new_backends')
  srcs += [ files('xrender.c', 'backend.c') ]

  # enable opengl
  if get_option('opengl')
    srcs += [ files('gl/gl_common.c', 'gl/glx.c') ]
    deps += [ dependency('gl', required: true) ]
    cflags += [ '-DGL_GLEXT_PROTOTYPES' ]
  endif
endif
