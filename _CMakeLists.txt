project(compton)
cmake_minimum_required(VERSION 2.8)

set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")

add_subdirectory(man)

set(compton_SRCS src/compton.c)

set(CMAKE_C_FLAGS_DEBUG "-ggdb")
set(CMAKE_C_FLAGS_RELEASE "-O2 -march=native")
set(CMAKE_C_FLAGS_RELWITHDBGINFO "-O2 -march=native -ggdb")

add_definitions("-Wall" "-std=c99")

# == Version ==
execute_process(COMMAND sh -c "echo -n \\\"git-$(git describe --always --dirty)-$(git log -1 --date=short --pretty=format:%cd)\\\""
	WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
	OUTPUT_VARIABLE COMPTON_VERSION)
add_definitions("-DCOMPTON_VERSION=${COMPTON_VERSION}")

# == Options ==

include(CMakeDependentOption)

option(CONFIG_REGEX_PCRE "Enable PCRE regular expression support for blacklist entries (requires libpcre)" ON)
if (CONFIG_REGEX_PCRE)
	add_definitions("-DCONFIG_REGEX_PCRE")
endif ()

option(CONFIG_REGEX_PCRE_JIT "Use JIT support of libpcre)" ON)
if (CONFIG_REGEX_PCRE_JIT)
	add_definitions("-DCONFIG_REGEX_PCRE_JIT")
endif ()

option(CONFIG_LIBCONFIG "Enable configuration file parsing using libconfig" ON)
if (CONFIG_LIBCONFIG)
	add_definitions("-DCONFIG_LIBCONFIG")
endif ()

option(CONFIG_VSYNC_DRM "Enable DRM VSync support" ON)
if (CONFIG_VSYNC_DRM)
	add_definitions("-DCONFIG_VSYNC_DRM")
endif ()

option(CONFIG_VSYNC_OPENGL "Enable OpenGL support" ON)
if (CONFIG_VSYNC_OPENGL)
	add_definitions("-DCONFIG_VSYNC_OPENGL")
	list(APPEND compton_SRCS src/opengl.c)
endif ()

CMAKE_DEPENDENT_OPTION(CONFIG_VSYNC_OPENGL_GLSL
	"Enable GLSL support (GLX background blur, etc.)" ON
	"CONFIG_VSYNC_OPENGL" OFF)
if (CONFIG_VSYNC_OPENGL_GLSL)
	add_definitions("-DCONFIG_VSYNC_OPENGL_GLSL")
endif ()

CMAKE_DEPENDENT_OPTION(CONFIG_VSYNC_OPENGL_FBO
	"Enable OpenGL FBO support (GLX multi-pass blur, etc.)" ON
	"CONFIG_VSYNC_OPENGL" OFF)
if (CONFIG_VSYNC_OPENGL_FBO)
	add_definitions("-DCONFIG_VSYNC_OPENGL_FBO")
endif ()

CMAKE_DEPENDENT_OPTION(CONFIG_VSYNC_OPENGL_VBO
	"Enable OpenGL VBO support (does nothing right now)" ON
	"CONFIG_VSYNC_OPENGL" OFF)
if (CONFIG_VSYNC_OPENGL_VBO)
	add_definitions("-DCONFIG_VSYNC_OPENGL_VBO")
endif ()

option(CONFIG_XINERAMA "Enable additional Xinerama features" ON)
if (CONFIG_XINERAMA)
	add_definitions("-DCONFIG_XINERAMA")
endif ()

option(CONFIG_DBUS "Enable D-Bus support" ON)
if (CONFIG_DBUS)
	add_definitions("-DCONFIG_DBUS")
	list(APPEND compton_SRCS src/dbus.c)
endif ()

option(CONFIG_XSYNC "Enable X Sync support (X Sync fence)" ON)
if (CONFIG_XSYNC)
	add_definitions("-DCONFIG_XSYNC")
endif ()

option(CONFIG_C2 "Enable matching system" ON)
if (CONFIG_C2)
	add_definitions("-DCONFIG_C2")
	list(APPEND compton_SRCS src/c2.c)
endif ()

add_executable(compton ${compton_SRCS})

# == Find libraries ==

target_link_libraries(compton "-lm" "-lrt")

if (CONFIG_VSYNC_OPENGL)
	target_link_libraries(compton "-lGL")
endif ()

include(FindPkgConfig)

# --- Find X11 libs ---
set(X11_FIND_REQUIRED 1)
include(FindX11)

macro(X11LIB_CHK lib)
	if (NOT X11_${lib}_FOUND)
		message(FATAL_ERROR "Could not find lib${lib}.")
	endif ()
	target_link_libraries(compton ${X11_${lib}_LIB})
endmacro ()

target_link_libraries(compton ${X11_X11_LIB})
X11LIB_CHK(Xcomposite)
X11LIB_CHK(Xdamage)
X11LIB_CHK(Xext)
X11LIB_CHK(Xfixes)
X11LIB_CHK(Xrender)
X11LIB_CHK(Xrandr)
if (CONFIG_XINERAMA)
	X11LIB_CHK(Xinerama)
endif ()

# --- Find libpcre ---
if (CONFIG_REGEX_PCRE)
	pkg_check_modules(LIBPCRE REQUIRED libpcre>=8.12)
	add_definitions(${LIBPCRE_CFLAGS})
	target_link_libraries(compton ${LIBPCRE_LDFLAGS})
endif ()

# --- Find libconfig ---
if (CONFIG_LIBCONFIG)
	pkg_check_modules(LIBCONFIG REQUIRED libconfig>=1.3.2)
	add_definitions(${LIBCONFIG_CFLAGS})
	target_link_libraries(compton ${LIBCONFIG_LDFLAGS})
	if (LIBCONFIG_VERSION VERSION_LESS 1.4)
		add_definitions(-DCONFIG_LIBCONFIG_LEGACY)
		message(STATUS "libconfig-1.3* detected. Enable legacy mode.")
	endif ()
endif ()

# --- Find libdbus ---
if (CONFIG_DBUS)
	pkg_check_modules(DBUS REQUIRED dbus-1)
	add_definitions(${DBUS_CFLAGS})
	target_link_libraries(compton ${DBUS_LDFLAGS})
endif ()

# --- Find libdrm ---
if (CONFIG_VSYNC_DRM)
	pkg_check_modules(LIBDRM REQUIRED libdrm)
	# We only use its header file
	add_definitions(${LIBDRM_CFLAGS})
endif ()

# == Install ==
include(GNUInstallDirs)

install(TARGETS compton
	DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT prog)
install(FILES "${PROJECT_SOURCE_DIR}/bin/compton-trans"
	DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT prog)
install(FILES
	"${PROJECT_SOURCE_DIR}/README.md"
	"${PROJECT_SOURCE_DIR}/LICENSE"
	DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT doc)

# == CPack ==

if (NOT CPACK_SYSTEM_NAME)
	set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(CPACK_PACKAGE_DESCRIPTION "A lightweight X compositing window manager, fork of xcompmgr-dana.")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight X compositing window manager")
set(CPACK_PACKAGE_CONTACT "nobody <devnull@example.com>")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/release")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/desc.txt")

# --- Package config ---
set(CPACK_GENERATOR "TBZ2" "DEB" "RPM")
set(CPACK_MONOLITHIC_INSTALL 1)
set(CPACK_STRIP_FILES 1)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")

# --- Source package config ---
set(CPACK_SOURCE_GENERATOR "TBZ2" "DEB" "RPM")
set(CPACK_SOURCE_IGNORE_FILES
	"/\\\\."
	"\\\\.bak$"
	"\\\\.o$"
	"\\\\~$"
	"\\\\.plist$"
	"/compton$"
	# Generated package files
	"\\\\.tar\\\\.gz$"
	"\\\\.tar\\\\.bz2$"
	"\\\\.deb$"
	"\\\\.rpm$"
	"compton-.*\\\\.sh$"
	# CMake files
	"/Makefile$"
	"/CMakeFiles/"
	"\\\\.cmake$"
	"CMakeCache\\\\.txt$"
	"/build/"
	"/release/"
	"\\\\.diff$"
	"/oprofile_data/"
	"/install_manifest\\\\.txt$"
)

# --- DEB package config ---
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.15), libconfig9, libdbus-1-3 (>= 1.1.1), libgl1-mesa-glx | libgl1 | libgl1-nvidia-glx | libgl1-fglrx-glx, libpcre3 (>= 8.10), libx11-6, libxcomposite1 (>= 1:0.3-1), libxdamage1 (>= 1:1.1), libxext6, libxfixes3, libxrandr2 (>= 4.3), libxrender1, libxinerama1")

# --- RPM package config ---
set(CPACK_RPM_PACKAGE_LICENSE "unknown")
set(CPACK_RPM_PACKAGE_REQUIRES "/bin/sh,libGL.so.1,libX11.so.6,libXcomposite.so.1,libXdamage.so.1,libXext.so.6,libXfixes.so.3,libXrandr.so.2,libXrender.so.1,libc.so.6,libconfig.so.9,libdbus-1.so.3,libm.so.6,libpcre.so.1")

include(CPack)
