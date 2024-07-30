mans = ['picom.1', 'picom-inspect.1', 'picom-trans.1']
if get_option('with_docs')
	a2x = find_program('asciidoctor')
	foreach m : mans
		custom_target(m, output: [m], input: [m+'.asciidoc'],
		              command: [a2x, '-a',
		                        'picom-version='+version,
		                        '--backend', 'manpage', '@INPUT@', '-D',
		                        meson.current_build_dir()],
		              install: true,
		              install_dir: join_paths(get_option('mandir'), 'man1'))
	endforeach
endif
