# tev — Tom94's EXR Viewer

An inspection and comparison tool for images with high dynamic range (HDR). __tev__ allows viewing images through various tonemapping operators and inspecting exact pixel values. Often, it is important to find exact differences between pairs of images. For this purpose, __tev__ allows rapidly switching between opened images and visualizing various error metrics (L1, L2, and relative versions thereof). To avoid clutter, opened images and their layers can be filtered by keywords.

While the predominantly supported file format is OpenEXR certain other types of images can also be loaded. The following file formats are currently supported:
- __EXR__ (via [OpenEXR](https://github.com/wjakob/openexr))
- __HDR__, BMP, GIF, JPEG, PIC, PNG, PNM, PSD, TGA (via [stb_image](https://github.com/wjakob/nanovg/blob/master/src/stb_image.h))
    - stb_image only supports [subsets](https://github.com/wjakob/nanovg/blob/master/src/stb_image.h#L23) of each of the aforementioned file formats.
    - Low-dynamic-range (LDR) images are "promoted" to HDR through an inverse gamma tonemappinng operator, where `gamma==2.2`.

## Screenshot

![Screenshot](https://raw.githubusercontent.com/Tom94/tev/master/resources/screenshot.png)
_A false-color comparison two multi-layer OpenEXR images of a beach ball. Image courtesy of [openexr-images](https://github.com/openexr/openexr-images)._

## Usage

Images can be opened via __tev__'s GUI or via the command line.
```sh
$ tev some-image.exr some-other-image.exr
```

By default, all _layers_ and _channels_ are loaded, but individual layers or channels can also be specified.
```sh
$ tev :some-layer some-image.exr :some-layer.some-channel yet-another-image.exr
```

For a list of all valid command-line arguments simply invoke
```sh
$ tev -h
```
and from within __tev__, press _h_ or click the little help icon in the top-right corner of the side bar to get an overview of all keybindings.

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

On Mac OS X and most Linux distributions [CMake](https://cmake.org/) can be obtained via a package manager ([Homebrew](https://brew.sh/) on Mac OS X, apt on Ubuntu/Debian, etc.). Most Linux distributions additionally require _xorg_, _gl_, and _zlib_ development packages and _zenity_. On Ubuntu/Debian simply call
```sh
$ apt-get install cmake xorg-dev libglu1-mesa-dev zlib1g-dev zenity
```

Once all dependencies are installed, create a new directory to contain build artifacts, enter it, and then invoke [CMake](https://cmake.org/) with the root __tev__ folder as argument as shown in the following example:
```sh
$ mkdir build
$ cd build
$ cmake ..
```

Afterwards, __tev__ can be built and installed via
```sh
$ make -j
$ make install
```

### Windows

On Windows, precompiled binaries of CMake can be found [here](https://cmake.org/download/). After installing CMake, open the included GUI application and point it to the root directory of __tev__. CMake will then generate [Visual Studio](https://www.visualstudio.com/) project files for compiling __tev__. Make sure you select at least Visual Studio 2015 (Win64) or higher!

## License

__tev__ is available under the BSD 3-clause license, which you can find in the `LICENSE.md` file. [TL;DR](https://tldrlegal.com/license/bsd-3-clause-license-(revised)) you can do almost whatever you want as long as you include the original copyright and license notice in any copy of the software and the source code.
