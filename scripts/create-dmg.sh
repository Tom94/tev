#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})"
RESULT="../tev.dmg"

test -f $RESULT && rm $RESULT
./create-dmg/create-dmg --window-size 500 300 --icon-size 96 --volname "tev Installer" --app-drop-link 360 105 --icon tev.app 130 105 $RESULT /Applications/tev.app
