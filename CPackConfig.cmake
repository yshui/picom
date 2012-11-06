# == Environment ==
if (NOT CPACK_SYSTEM_NAME)
	set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_PROCESSOR}")
	if (CPACK_SYSTEM_NAME STREQUAL "x86_64")
		set(CPACK_SYSTEM_NAME "amd64")
	endif ()
endif ()

# == Basic information ==
set(CPACK_PACKAGE_NAME "compton")
set(CPACK_PACKAGE_VENDOR "chjj")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set(CPACK_PACKAGE_DESCRIPTION "A lightweight X compositing window manager, fork of xcompmgr-dana.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight X compositing window manager")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_PACKAGE_CONTACT "nobody <devnull@example.com>")
set(CPACK_INSTALL_COMMANDS "env PREFIX=build make install")

# == Package config ==
set(CPACK_INSTALLED_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/build" "usr")
set(CPACK_GENERATOR "TBZ2" "DEB" "RPM")
set(CPACK_RESOURCE_FILE_LICENSE "LICENSE")
set(CPACK_RESOURCE_FILE_README "README.md")
set(CPACK_STRIP_FILES 1)

# == DEB package config ==
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CPACK_SYSTEM_NAME}")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
# The dependencies are unreliable, just an example here
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libx11-6, libxext6, libxcomposite1, libxrender1, libxdamage1, libxfixes3, libpcre3, libconfig8, libdrm2")

# == RPM package config ==
# The dependencies are unreliable, just an example here
set(CPACK_RPM_PACKAGE_REQUIRES "libx11, libxext, libxcomposite, libxrender, libxdamage, libxfixes, libpcre, libconfig, libdrm")

# == Source package config ==
set(CPACK_SOURCE_GENERATOR "TBZ2 DEB RPM")
