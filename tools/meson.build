executable(
	'animgen',
	'animgen.c',
	dependencies: [ base_deps, libconfig_dep, test_h_dep, cc.find_library('m')],
	link_with: [libtools],
	build_by_default: false,
	include_directories: picom_inc,
)
