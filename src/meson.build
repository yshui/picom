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
               'options.c', 'event.c', 'cache.c', 'atom.c', 'file_watch.c', 'statistics.c',
               'vblank.c', 'transition.c', 'wm.c', 'renderer/layout.c', 'renderer/command_builder.c',
               'renderer/renderer.c', 'renderer/damage.c', 'config_libconfig.c', 'inspect.c', 'script.c', 'api.c') ]
picom_inc = include_directories(['.', '../include'])

cflags = []

required_xcb_packages = [
	'xcb', 'xcb-composite', 'xcb-damage', 'xcb-glx', 'xcb-present', 'xcb-randr',
	'xcb-render', 'xcb-shape', 'xcb-sync', 'xcb-xfixes'
]

# Some XCB packages are here because their versioning differs (see check below).
required_packages = [
	'pixman-1', 'x11', 'x11-xcb', 'xcb-image', 'xcb-renderutil', 'xcb-util',
	'xext', 'threads',
]

foreach i : required_packages
	base_deps += [dependency(i, required: true)]
endforeach

foreach i : required_xcb_packages
	base_deps += [dependency(i, version: '>=1.12.0', required: true)]
endforeach
base_deps += [dependency('libconfig', version: '>=1.7', required: true)]

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
	srcs += [ 'opengl.c' ]
endif

if get_option('dbus')
	cflags += ['-DCONFIG_DBUS']
	deps += [dependency('dbus-1', required: true)]
	srcs += [ 'dbus.c', 'rtkit.c' ]
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
  install: true, include_directories: picom_inc,
  export_dynamic: true, gnu_symbol_visibility: 'hidden')

if get_option('unittest')
	test('picom unittest', picom, args: [ '--unittest' ])
endif

install_symlink('picom-inspect', install_dir: 'bin', pointing_to: 'picom')

if cc.has_argument('-fsanitize=fuzzer')
  c2_fuzz = executable('c2_fuzz', srcs + ['fuzzer/c2.c'],
	c_args: cflags + ['-fsanitize=fuzzer', '-DCONFIG_FUZZER'],
	link_args: ['-fsanitize=fuzzer'],
	dependencies: [ base_deps, deps, test_h_dep ],
	build_by_default: false,
	install: false, include_directories: picom_inc
  )
endif
