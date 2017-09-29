#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})"

echo "Building backwards-compatible tev..."
mkdir build-dmg && cd build-dmg
MACOSX_DEPLOYMENT_TARGET=10.9
cmake \
    -DCMAKE_OSX_SYSROOT=/Users/tom94/Projects/MacOSX-SDKs/MacOSX10.9.sdk/ \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9 \
    ../..
make -j
cd ..

echo "Creating dmg..."
RESULT="../tev.dmg"
test -f $RESULT && rm $RESULT
./create-dmg/create-dmg --window-size 500 300 --icon-size 96 --volname "tev Installer" --app-drop-link 360 105 --icon tev.app 130 105 $RESULT ./build-dmg/tev.app

echo "Removing temporary build dir..."
rm -rf build-dmg
