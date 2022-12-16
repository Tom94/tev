# tev — The EXR Viewer &nbsp;&nbsp; ![](https://github.com/tom94/tev/workflows/CI/badge.svg)

High dynamic range (HDR) image comparison tool for graphics people. __tev__ allows viewing images through various tonemapping operators and inspecting the values of individual pixels. To compare images, <strong>tev</strong> can rapidly flip between them and visualize various error metrics.

__tev__ can display true HDR on Apple extended dynamic range (EDR) and 10-bit displays.

The following file formats are supported:
- __EXR__ (via [OpenEXR](https://github.com/wjakob/openexr))
- __HDR__, __PNG__, __JPEG__, BMP, GIF, PIC, PNM, PSD, TGA (via [stb_image](https://github.com/wjakob/nanovg/blob/master/src/stb_image.h))
- __PFM__ (compatible with [Netbpm](http://www.pauldebevec.com/Research/HDR/PFM/))
- __QOI__ (via [qoi](https://github.com/phoboslab/qoi). Shoutout to [Tiago Chaves](https://github.com/laurelkeys) for adding support!)
- __DDS__ (via [DirectXTex](https://github.com/microsoft/DirectXTex); Windows only. Shoutout to [Craig Kolb](https://github.com/cek) for adding support!)
    - Supports BC1-BC7 compressed formats.

## Screenshot

![Screenshot](https://raw.githubusercontent.com/Tom94/tev/master/resources/screenshot.png)
_A false-color comparison of two multi-layer OpenEXR images of a beach ball. Image courtesy of [openexr-images](https://github.com/openexr/openexr-images)._

## Usage

### Graphical User Interface

Images can be opened via a file dialog or by dragging them into __tev__.
They can be reloaded, closed, or filtered at any time, so don't worry about opening more images than exactly needed.

Select an image by left-clicking it, and optionally select a reference image to compare the current selection to by right-clicking.
For convenience, the current selection can be moved with the Up/Down or the 1-9 keys. For a comprehensive list of keyboard shortcuts click the little "?" icon at the top (or press "h").

If the interface seems overwhelming, you can hover any controls to view an explanatory tooltip.

### Command Line

__tev__ takes images as positional command-line arguments:
```sh
$ tev foo.exr bar.exr
```

By default, all layers and channels are loaded, but individual layers or channels can also be specified. In the following example, the *depth* layer of *foo.exr* and the *r*, *g*, and *b* channels of *foo.exr* and *bar.exr* are loaded.
```sh
$ tev :depth foo.exr :r,g,b foo.exr bar.exr
```

Other command-line arguments exist (e.g. for starting __tev__ with a pre-set exposure value). For a list of all arguments simply invoke
```sh
$ tev -h
```

### Over the Network

__tev__ can also be controlled remotely over the network using a simple TCP-based protocol.

The `--host` argument specifies the IP and port __tev__ is listening to. By default, __tev__ only accepts connections from localhost (`127.0.0.1:14158`), which is useful, for example, as frontend for a supported renderer like [pbrt version 4](https://github.com/mmp/pbrt-v4).

The following operations exist:

| Operation | Function
| :--- | :----------
| `OpenImage` | Opens an image from a specified path on the machine __tev__ is running on.
| `CreateImage` | Creates a blank image with a specified name, size, and set of channels. If an image with the specified name already exists, it is overwritten.
| `UpdateImage` | Updates the pixels in a rectangular region.
| `CloseImage` | Closes a specified image.
| `ReloadImage` | Reloads an image from a specified path on the machine __tev__ is running on.
| `VectorGraphics` | Draws vector graphics over a specified image.

__tev__'s network protocol is already implemented in the following languages:
- [Python](src/python/tev.py) by Tomáš Iser
- [Rust](https://crates.io/crates/tev_client) by Karel Peeters


If using these implementations is not an option, it's easy to write your own one. Each packet has the simple form
```
[uint32_t total_length_in_bytes][byte operation_type][byte[] operation_specific_payload]
```
where integers are encoded in little endian.

There are helper functions in [Ipc.cpp](src/Ipc.cpp) (`IpcPacket::set*`) that show exactly how each packet has to be assembled. These functions do not rely on external dependencies, so it is recommended to copy and paste them into your project for interfacing with __tev__.


## Obtaining tev

### macOS / Windows

Pre-built binaries for Windows (32-bit and 64-bit) and macOS (64-bit, arm) are available on the [releases page](https://github.com/Tom94/tev/releases).

On macOS, __tev__ can also be installed via homebrew:
```bash
brew install --cask tev
```

### Linux

- Pre-built portable Linux binaries (64-bit) are available on the [releases page](https://github.com/Tom94/tev/releases). See [how to run AppImages](https://appimage.org/).
- Archlinux: available on the [Arch User Repository](https://aur.archlinux.org/packages/tev/)

## Building tev

All that is required for building __tev__ is a C++20-compatible compiler. Begin by cloning this repository and all its submodules using the following command:
```sh
$ git clone --recursive https://github.com/Tom94/tev
```

__tev__ uses [CMake](https://cmake.org/) as its build system. The following sections detail how it should be used on various operating systems.

### macOS / Linux

On macOS and most Linux distributions [CMake](https://cmake.org/) can be obtained via a package manager ([Homebrew](https://brew.sh/) on macOS, apt on Ubuntu/Debian, etc.). Most Linux distributions additionally require _xorg_, _gl_, and _zlib_ development packages and _zenity_. On Ubuntu/Debian simply call
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

On Linux you can package a portable [AppImage](https://appimage.org/) with
```sh
$ make package
```

### Windows

On Windows, install [CMake](https://cmake.org/download/), open the included GUI application, and point it to the root directory of __tev__. CMake will then generate [Visual Studio](https://www.visualstudio.com/) project files for compiling __tev__. Make sure you select at least Visual Studio 2019 or higher!

## License

__tev__ is available under the BSD 3-clause license, which you can find in the `LICENSE.md` file. [TL;DR](https://tldrlegal.com/license/bsd-3-clause-license-(revised)) you can do almost whatever you want as long as you include the original copyright and license notice in any copy of the software and the source code.
