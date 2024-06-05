# enable xrender
srcs += [
  files(
    'dummy/dummy.c',
    'backend.c',
    'backend_common.c',
    'driver.c',
  ),
]
subdir('xrender')
# enable opengl
if get_option('opengl')
  subdir('gl')
endif
