mans = ['compton.1', 'compton-trans.1']
if get_option('with_docs')
	a2x = find_program('a2x')
	foreach m : mans
		custom_target(m, output: [m], input: [m+'.asciidoc'],
		              command: [a2x, '-a',
		                        'compton-version='+version,
		                        '--format', 'manpage', '@INPUT@', '-D',
		                        meson.current_build_dir()],
		              install: true, install_dir: 'share/man/man1/')
	endforeach
endif
