libev = dependency('libev', required: false)
if not libev.found()
	libev = cc.find_library('ev')
endif
base_deps = [cc.find_library('m'), libev]

srcs = [
	files(
		'api.c',
		'atom.c',
		'c2.c',
		'config.c',
		'config_libconfig.c',
		'diagnostic.c',
		'event.c',
		'inspect.c',
		'log.c',
		'options.c',
		'picom.c',
		'render.c',
		'vblank.c',
		'vsync.c',
		'x.c',
	),
]
picom_inc = include_directories(['.', '../include'])

cflags = []

required_xcb_packages = [
	'xcb',
	'xcb-composite',
	'xcb-damage',
	'xcb-glx',
	'xcb-present',
	'xcb-randr',
	'xcb-render',
	'xcb-shape',
	'xcb-sync',
	'xcb-xfixes',
]

# Some XCB packages are here because their versioning differs (see check below).
required_packages = [
	'pixman-1',
	'x11',
	'x11-xcb',
	'xcb-image',
	'xcb-renderutil',
	'xcb-util',
	'threads',
]

foreach i : required_packages
	base_deps += [dependency(i, required: true)]
endforeach

foreach i : required_xcb_packages
	base_deps += [dependency(i, version: '>=1.12.0', required: true)]
endforeach

libconfig_dep = dependency('libconfig', version: '>=1.7', required: false)

if not libconfig_dep.found()
	warning('Trying to clone and build libconfig as a subproject.')

	cmake = import('cmake')
	sub_libconfig_opts = cmake.subproject_options()
	sub_libconfig_opts.add_cmake_defines(
		{
			'BUILD_SHARED_LIBS': false,
		},
	)
	sub_libconfig_opts.set_install(false)
	sub_libconfig = cmake.subproject('libconfig', options: sub_libconfig_opts)
	base_deps += [sub_libconfig.dependency('config')]
else
	base_deps += [libconfig_dep]
endif

if not cc.has_header('uthash.h')
	error('Dependency uthash not found')
endif

deps = []

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
	cflags += ['-DCONFIG_OPENGL']
	deps += [dependency('epoxy', required: true)]
	srcs += ['opengl.c']
endif

if get_option('dbus')
	cflags += ['-DCONFIG_DBUS']
	deps += [dependency('dbus-1', required: true)]
	srcs += ['dbus.c', 'rtkit.c']
endif

if get_option('xrescheck')
	cflags += ['-DDEBUG_XRC']
	srcs += ['xrescheck.c']
endif

if get_option('unittest')
	cflags += ['-DUNIT_TEST']
endif

host_system = host_machine.system()
if host_system == 'linux'
	cflags += ['-DHAS_INOTIFY']
elif (
	host_system == 'freebsd'
	or host_system == 'netbsd'
	or host_system == 'dragonfly'
	or host_system == 'openbsd'
)
	cflags += ['-DHAS_KQUEUE']
endif

subdir('backend')
subdir('wm')
subdir('renderer')
subdir('transition')
subdir('utils')

dl_dep = []
if not cc.has_function('dlopen')
	dl_dep = [cc.find_library('dl', required: true)]
endif

libtools = static_library(
	'libtools',
	[
		'log.c',
		'utils/dynarr.c',
		'utils/misc.c',
		'utils/str.c',
		'transition/curve.c',
		'transition/script.c',
	],
	include_directories: picom_inc,
	dependencies: [test_h_dep],
	install: false,
	build_by_default: false,
)

picom = executable(
	'picom',
	srcs,
	c_args: cflags,
	dependencies: [base_deps, deps, test_h_dep] + dl_dep,
	install: true,
	include_directories: picom_inc,
	export_dynamic: true,
	gnu_symbol_visibility: 'hidden',
)

if get_option('unittest')
	test('picom unittest', picom, args: ['--unittest'])
endif

install_symlink('picom-inspect', install_dir: 'bin', pointing_to: 'picom')

if cc.has_argument('-fsanitize=fuzzer')
	c2_fuzz = executable(
		'c2_fuzz',
		srcs + ['fuzzer/c2.c'],
		c_args: cflags + ['-fsanitize=fuzzer', '-DCONFIG_FUZZER'],
		link_args: ['-fsanitize=fuzzer'],
		dependencies: [base_deps, deps, test_h_dep],
		build_by_default: false,
		install: false,
		include_directories: picom_inc,
	)
endif
