#!/bin/bash

realpath() {
    # If the path starts with a slash, then it is already
    # absolute. Otherwise, simply prepend current dir.
    [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
}

# Transform all path arguments to absolute paths.
# This is required, since opening an app in macOS
# does not let us control the working directory of
# the app, and relative paths may thus not work.
paths=()
for var in "$@"
do
    # Command-line arguments starting with - or :
    # are not interpreted as paths by tev, so we
    # should not expand them.
    if  [[ $var == -* ]] || [[ $var == :* ]]; then
        paths+=($var)
    else
        paths+=("$(realpath $var)")
    fi
done

open -n -a tev --args "${paths[@]}"
