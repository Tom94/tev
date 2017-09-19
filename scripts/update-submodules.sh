#!/bin/bash

cd "$(dirname ${BASH_SOURCE[0]})/.."
git submodule update --recursive
git submodule sync --recursive
git submodule update --recursive
