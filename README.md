# tview

An inspection and comparison tool for images with high dynamic range.

## Building tview

All that is required for building __tview__ is a C++14-compatible compiler. Begin by cloning this repository and all its submodules using the following command:
```sh
$ git clone --recursive https://github.com/Tom94/tview/
```

If you accidentally omitted the `--recursive` flag when cloning this repository you can initialize the submodules like so:
```sh
$ git submodule update --init --recursive
```

tview uses [CMake](https://cmake.org/) as its build system. The following sections detail how it should be used on various operating systems.

### Mac OS X / Linux

On Mac OS X and most Linux distributions [CMake](https://cmake.org/) can be obtained via a package manager ([Homebrew](https://brew.sh/) on Mac OS X, apt on Ubuntu/Debian, etc.). Once you obtained [CMake](https://cmake.org/), create a new directory to contain build artifacts, enter it, and then invoke [CMake](https://cmake.org/) with the root __tview__ folder as argument as shown in the following example:
```sh
$ mkdir build
$ cd build
$ cmake ..
```

### Windows

On Windows, precompiled binaries of CMake can be found [here](https://cmake.org/download/). After installing CMake, open the included GUI application and point it to the root directory of __tview__. CMake will then generate [Visual Studio](https://www.visualstudio.com/) project files for compiling __tview__. Make sure you select at least Visual Studio 2015 or higher, otherwise the compiler will not have sufficient C++14 support!

## License

__tview__ is available under the BSD 3-clause license, which you can find in the `LICENSE.md` file. [TL;DR](https://tldrlegal.com/license/bsd-3-clause-license-(revised)) you can do almost whatever you want as long as you include the original copyright and license notice in any copy of the software and the source code.
