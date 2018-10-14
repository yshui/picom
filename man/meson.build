mans = ['compton.1', 'compton-trans.1']
a2x = find_program('a2x')
foreach m : mans
	custom_target(m, output: [m], input: [m+'.asciidoc'],
	              command: [a2x, '--format', 'manpage', '@INPUT@',
	                        '-D', meson.current_build_dir()],
	              install: true, install_dir: 'share/man/man1/')
endforeach
