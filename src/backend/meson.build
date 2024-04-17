# enable xrender
srcs += [
  files(
    'dummy/dummy.c',
    'xrender/xrender.c',
    'backend.c',
    'backend_common.c',
    'driver.c',
  ),
]

# enable opengl
if get_option('opengl')
  srcs += [
    files('gl/blur.c', 'gl/egl.c', 'gl/gl_common.c', 'gl/glx.c', 'gl/shaders.c'),
  ]
endif
