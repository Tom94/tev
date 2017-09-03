# tev — Tom94's EXR Viewer

A _work-in-progress_ inspection and comparison tool for images with high dynamic range (HDR). As the name suggests, the predominantly supported file format is OpenEXR, however certain other types of images can also be loaded. The following file formats are currently supported:
- __EXR__ (via [OpenEXR](https://github.com/wjakob/openexr))
- __HDR__, BMP, GIF, JPEG, PIC, PNG, PNM, PSD, TGA (via [stb_image](https://github.com/wjakob/nanovg/blob/master/src/stb_image.h))
    - stb_image only supports [subsets](https://github.com/wjakob/nanovg/blob/master/src/stb_image.h#L23) of each of the aforementioned file formats.
    - Low-dynamic-range (LDR) images are "promoted" to HDR through an inverse gamma tonemappinng operator, where `gamma==2.2`.

## Building tev

All that is required for building __tev__ is a C++14-compatible compiler. Begin by cloning this repository and all its submodules using the following command:
```sh
$ git clone --recursive https://github.com/Tom94/tev
```

If you accidentally omitted the `--recursive` flag when cloning this repository you can initialize the submodules like so:
```sh
$ git submodule update --init --recursive
```

__tev__ uses [CMake](https://cmake.org/) as its build system. The following sections detail how it should be used on various operating systems.

### Mac OS X / Linux

On Mac OS X and most Linux distributions [CMake](https://cmake.org/) can be obtained via a package manager ([Homebrew](https://brew.sh/) on Mac OS X, apt on Ubuntu/Debian, etc.). Most Linux distributions additionally require _xorg_ and _gl_ development packages. On Ubuntu/Debian simply call
```sh
$ apt-get install cmake xorg-dev libglu1-mesa-dev zlib1g-dev
```

Once all dependencies are installed, create a new directory to contain build artifacts, enter it, and then invoke [CMake](https://cmake.org/) with the root __tev__ folder as argument as shown in the following example:
```sh
$ mkdir build
$ cd build
$ cmake ..
```

### Windows

On Windows, precompiled binaries of CMake can be found [here](https://cmake.org/download/). After installing CMake, open the included GUI application and point it to the root directory of __tev__. CMake will then generate [Visual Studio](https://www.visualstudio.com/) project files for compiling __tev__. Make sure you select at least Visual Studio 2015 or higher, otherwise the compiler will not have sufficient C++14 support!

## License

__tev__ is available under the BSD 3-clause license, which you can find in the `LICENSE.md` file. [TL;DR](https://tldrlegal.com/license/bsd-3-clause-license-(revised)) you can do almost whatever you want as long as you include the original copyright and license notice in any copy of the software and the source code.
