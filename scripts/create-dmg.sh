#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})"

echo "Building tev..."

BUILD_DIR="build-dmg"

mkdir $BUILD_DIR && cd $BUILD_DIR
MACOSX_DEPLOYMENT_TARGET=10.14
cmake \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.14 \
    -DTEV_DEPLOY=1 \
    ../..
make -j
cd ..

echo "Creating dmg..."
RESULT="../tev.dmg"
test -f $RESULT && rm $RESULT
./create-dmg/create-dmg --window-size 500 300 --icon-size 96 --volname "tev Mojave Installer" --app-drop-link 360 105 --icon tev.app 130 105 $RESULT $BUILD_DIR/tev.app

echo "Removing temporary build dir..."
rm -rf $BUILD_DIR
