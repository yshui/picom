#!/bin/sh

if [ ! -e "${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton" ]; then
	echo "Linking picom to ${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton"
	ln -s picom "${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton"
fi

if [ ! -e "${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton-trans" ]; then
	echo "Linking picom-trans to ${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton-trans"
	ln -s picom-trans "${MESON_INSTALL_DESTDIR_PREFIX}/bin/compton-trans"
fi
