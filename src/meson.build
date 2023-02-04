libev = dependency('libev', required: false)
if not libev.found()
	libev = cc.find_library('ev')
endif
base_deps = [
	cc.find_library('m'),
	libev
]

srcs = [ files('picom.c', 'win.c', 'c2.c', 'x.c', 'config.c', 'vsync.c', 'utils.c',
               'diagnostic.c', 'string_utils.c', 'render.c', 'kernel.c', 'log.c',
               'options.c', 'event.c', 'cache.c', 'atom.c', 'file_watch.c') ]
picom_inc = include_directories('.')

cflags = []

required_xcb_packages = [
	'xcb-render', 'xcb-damage', 'xcb-randr', 'xcb-sync', 'xcb-composite',
	'xcb-shape', 'xcb-xfixes', 'xcb-present', 'xcb-glx', 'xcb-dpms', 'xcb'
]

required_packages = [
	'x11', 'x11-xcb', 'xcb-renderutil', 'xcb-image', 'xext', 'pixman-1'
]

foreach i : required_packages
	base_deps += [dependency(i, required: true)]
endforeach

foreach i : required_xcb_packages
	base_deps += [dependency(i, version: '>=1.12.0', required: true)]
endforeach

if not cc.has_header('uthash.h')
  error('Dependency uthash not found')
endif

deps = []

if get_option('config_file')
	deps += [dependency('libconfig', version: '>=1.4', required: true)]

	cflags += ['-DCONFIG_LIBCONFIG']
	srcs += [ 'config_libconfig.c' ]
endif
if get_option('regex')
	pcre = dependency('libpcre2-8', required: true)
	cflags += ['-DCONFIG_REGEX_PCRE']
	deps += [pcre]
endif

if get_option('vsync_drm')
	cflags += ['-DCONFIG_VSYNC_DRM']
	deps += [dependency('libdrm', required: true)]
endif

if get_option('opengl')
	cflags += ['-DCONFIG_OPENGL', '-DGL_GLEXT_PROTOTYPES']
	deps += [dependency('gl', required: true), dependency('egl', required: true)]
	srcs += [ 'opengl.c' ]
endif

if get_option('dbus')
	cflags += ['-DCONFIG_DBUS']
	deps += [dependency('dbus-1', required: true)]
	srcs += [ 'dbus.c' ]
endif

if get_option('xrescheck')
	cflags += ['-DDEBUG_XRC']
	srcs += [ 'xrescheck.c' ]
endif

if get_option('unittest')
	cflags += ['-DUNIT_TEST']
endif

host_system = host_machine.system()
if host_system == 'linux'
	cflags += ['-DHAS_INOTIFY']
elif (host_system == 'freebsd' or host_system == 'netbsd' or
      host_system == 'dragonfly' or host_system == 'openbsd')
	cflags += ['-DHAS_KQUEUE']
endif

subdir('backend')

picom = executable('picom', srcs, c_args: cflags,
  dependencies: [ base_deps, deps, test_h_dep ],
  install: true, include_directories: picom_inc)

if get_option('unittest')
	test('picom unittest', picom, args: [ '--unittest' ])
endif
