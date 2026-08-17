#!/bin/bash
set -euo pipefail

cd /usr/src/vkQuake

rm -rf build/appimage

cmake -B build/appimage -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/appimage

cd Packaging/AppImage
rm -rf AppDir
rm -rf vkquake* vkQuake-*

# NO_STRIP keeps the debug info in the binary for symbolizing crash
# reports; the sections are not loaded at runtime
NO_STRIP=1 ./linuxdeploy-x86_64.AppImage \
	-e ../../build/appimage/vkquake --appdir=AppDir -d ../../Misc/vkquake.desktop \
	-i ../../Misc/vkQuake_256.png --icon-filename=vkquake --output appimage
