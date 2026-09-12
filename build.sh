#!/usr/bin/env bash
#
# build.sh - Build the picom compositor.
#
# Produces the picom binary at ./build/picom.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BINARY="${BUILD_DIR}/picom"
SOURCE_BINARY="${BUILD_DIR}/src/picom"

cd "${SCRIPT_DIR}"

# Ensure build dependencies are available.
for cmd in meson ninja gcc pkg-config; do
	if ! command -v "${cmd}" >/dev/null 2>&1; then
		echo "error: ${cmd} is required but not found in PATH" >&2
		exit 1
	fi
done

# Configure the build directory if it does not exist or is incomplete.
if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
	echo "==> Configuring picom (meson setup)"
	meson setup --buildtype=release "${BUILD_DIR}"
fi

# Compile.
echo "==> Building picom (ninja)"
ninja -C "${BUILD_DIR}"

# Ensure the binary ends up at ./build/picom.
if [ -f "${SOURCE_BINARY}" ]; then
	if [ "${SOURCE_BINARY}" != "${BINARY}" ]; then
		cp "${SOURCE_BINARY}" "${BINARY}"
	fi
fi

echo "==> Built: ${BINARY}"
