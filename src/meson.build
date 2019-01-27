base_deps = [
	cc.find_library('m'),
	cc.find_library('ev'),
	dependency('xcb', version: '>=1.9.2'),
]

srcs = [ files('compton.c', 'win.c', 'c2.c', 'x.c', 'config.c', 'utils.c',
               'diagnostic.c', 'string_utils.c', 'kernel.c', 'log.c',
               'options.c') ]
compton_inc = include_directories('.')

cflags = []


required_package = [
	'x11', 'x11-xcb', 'xcb-renderutil',
	'xcb-render', 'xcb-damage', 'xcb-randr', 'xcb-sync',
	'xcb-composite', 'xcb-shape', 'xcb-image',
	'xcb-xfixes', 'xcb-present', 'xext', 'pixman-1'
]

foreach i : required_package
	base_deps += [dependency(i, required: true)]
endforeach

deps = []

if get_option('xinerama')
	deps += [dependency('xcb-xinerama', required: true)]
	cflags += ['-DCONFIG_XINERAMA']
endif

if get_option('config_file')
	deps += [dependency('libconfig', version: '>=1.4', required: true),
	         dependency('libxdg-basedir', required: true)]
	cflags += ['-DCONFIG_LIBCONFIG']
	srcs += [ 'config_libconfig.c' ]
endif
if get_option('regex')
	pcre = dependency('libpcre', required: true)
	cflags += ['-DCONFIG_REGEX_PCRE']
	if pcre.version().version_compare('>=8.20')
		cflags += ['-DCONFIG_REGEX_PCRE_JIT']
	endif
	deps += [pcre]
endif

if get_option('vsync_drm')
	cflags += ['-DCONFIG_VSYNC_DRM']
	deps += [dependency('libdrm', required: true)]
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

if get_option('opengl')
	cflags += ['-DCONFIG_OPENGL', '-DGL_GLEXT_PROTOTYPES']
	deps += [dependency('gl', required: true)]
endif
subdir('backend')

executable('compton', srcs, c_args: cflags,
  dependencies: [ base_deps, deps ],
  install: true, include_directories: compton_inc)
