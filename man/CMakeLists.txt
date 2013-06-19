option(NEW_DOC "Build new man pages and HTML documentation" ON)

# == Build documentation ==
# Stolen from https://issues.apache.org/jira/secure/attachment/12455612/AVRO-470.patch
if (NEW_DOC)
	set (MAN_SRC
		compton.1.asciidoc
		compton-trans.1.asciidoc
	)

	find_program(ASCIIDOC_EXEC asciidoc)
	find_program(ASCIIDOC_A2X_EXEC a2x)
	if (ASCIIDOC_EXEC AND ASCIIDOC_A2X_EXEC)
		foreach(_file ${MAN_SRC})
			# get_filename_component() does not handle ".1.asciidoc"
			# correctly
			string(REPLACE ".asciidoc" "" _file_we "${_file}")
			set(_file_path "${CMAKE_CURRENT_SOURCE_DIR}/${_file}")
			set(_html_out "${_file_we}.html")
			set(_man_out "${_file_we}")
			add_custom_target(compton_man_${_file_we} ALL
				COMMAND ${ASCIIDOC_A2X_EXEC} --format manpage
					"${_file_path}"
				DEPENDS "${_file_path}"
			)
			add_custom_command(
				OUTPUT "${_html_out}"
				COMMAND ${ASCIIDOC_EXEC} -o "${_html_out}" "${_file_path}"
				DEPENDS "${_file_path}"
			)
			add_custom_target(compton_html_${_file_we} ALL
				DEPENDS "${_html_out}"
			)
		endforeach(_file)
	else(ASCIIDOC_EXEC AND ASCIIDOC_A2X_EXEC)
		message(WARNING "asciidoc/a2x not found. New man pages and HTML documentation will not be built.")
	endif(ASCIIDOC_EXEC AND ASCIIDOC_A2X_EXEC)
endif(NEW_DOC)

# == Install ==
include(GNUInstallDirs)

install(FILES
	"compton.1"
	"compton-trans.1"
	DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT doc)
