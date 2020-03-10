#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})"

echo "Building backwards-compatible tev..."

BUILD_DIR="build-dmg"

mkdir $BUILD_DIR && cd $BUILD_DIR
MACOSX_DEPLOYMENT_TARGET=10.10
cmake \
    -DCMAKE_OSX_SYSROOT=/Users/tom94/Projects/MacOSX-SDKs/MacOSX10.10.sdk/ \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.10 \
    -DTEV_DEPLOY=1 \
    ../..
make -j
cd ..

echo "Creating dmg..."
RESULT="../tev-pre-mojave.dmg"
test -f $RESULT && rm $RESULT
./create-dmg/create-dmg --window-size 500 300 --icon-size 96 --volname "tev Installer" --app-drop-link 360 105 --icon tev.app 130 105 $RESULT $BUILD_DIR/tev.app

echo "Removing temporary build dir..."
rm -rf $BUILD_DIR
