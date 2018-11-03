project('compton', 'c', version: '2')

cc = meson.get_compiler('c')

version = ''
git = find_program('git', required: false)
if git.found()
	gitv = run_command('git', 'describe')
	if gitv.returncode() == 0
		version = gitv.stdout().strip()
	endif
endif

if version == ''
	# use project version by default
	version = 'v'+meson.project_version()
endif

add_global_arguments('-std=c11', language: 'c')
add_global_arguments('-DCOMPTON_VERSION="'+version+'"', language: 'c')

if get_option('buildtype') == 'release'
  add_global_arguments('-DNDEBUG', language: 'c')
endif

if get_option('sanitize')
	sanitizers = ['address', 'undefined']
	if cc.has_argument('-fsanitize=integer')
		sanitizers += ['integer']
	endif
	if cc.has_argument('-fsanitize=nullability')
		sanitizers += ['nullability']
	endif
	add_global_arguments('-fsanitize='+','.join(sanitizers), language: 'c')
	add_global_link_arguments('-fsanitize='+','.join(sanitizers), language: 'c')
endif

add_global_arguments('-D_GNU_SOURCE', language: 'c')

warns = [ 'all', 'extra', 'no-unused-parameter', 'nonnull', 'shadow', 'implicit-fallthrough' ]
foreach w : warns
	if cc.has_argument('-W'+w)
		add_global_arguments('-W'+w, language: 'c')
	endif
endforeach

subdir('src')
subdir('man')

install_subdir('bin', install_dir: '')
install_data('compton.desktop', install_dir: 'share/applications')
install_data('media/icons/48x48/compton.png',
             install_dir: 'share/icons/hicolor/48x48/apps')
install_data('media/compton.svg',
             install_dir: 'share/icons/hicolor/scalable/apps')
