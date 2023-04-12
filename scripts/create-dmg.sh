#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})"

echo "Building tev..."

BUILD_DIR="build-dmg"

mkdir $BUILD_DIR
cd $BUILD_DIR
MACOSX_DEPLOYMENT_TARGET=10.15
cmake \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
    -DTEV_DEPLOY=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    ../.. || exit 1
make -j || exit 1
cd ..

echo "Creating dmg..."
RESULT="../tev.dmg"
test -f $RESULT
rm $RESULT
./create-dmg/create-dmg --window-size 500 300 --icon-size 96 --volname "tev Installer" --app-drop-link 360 105 --icon tev.app 130 105 $RESULT $BUILD_DIR/tev.app

echo "Removing temporary build dir..."
rm -rf $BUILD_DIR
