/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ImageViewer.h>
#include <tev/Ipc.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/GainMap.h>

#include <ImfThreading.h>
#include <args.hxx>

#include <utf8.h>

#ifdef __APPLE__
#    define GLFW_EXPOSE_NATIVE_COCOA
#    include <GLFW/glfw3native.h>
#endif

#include <charconv>
#include <iostream>
#include <thread>

using namespace args;
using namespace std;

namespace tev {

// Image viewer is a static variable to allow other parts of the program to easily schedule operations onto the main nanogui thread loop. In
// a truly modular program, this would never be required, but OpenGL's state-machine nature throws a wrench into modularity. Currently, the
// only use case is the destruction of OpenGL textures, which _must_ happen on the thread on which the GL context is "current".
static ImageViewer* sImageViewer = nullptr;
static atomic<bool> imageViewerIsReady = false;

void scheduleToMainThread(const function<void()>& fun) {
    if (imageViewerIsReady) {
        sImageViewer->scheduleToUiThread(fun);
    }
}

void redrawWindow() {
    if (imageViewerIsReady) {
        sImageViewer->redraw();
    }
}

// Stricter version of from_chars that only returns true if the entire input was consumed and no error occurred.
template <typename T> bool fromChars(const char* begin, const char* end, T&& value) {
    const auto result = from_chars(begin, end, std::forward<T>(value));
    return result.ec == errc{} && result.ptr == end;
}

template <typename T> bool fromChars(string_view s, T&& value) { return fromChars(s.data(), s.data() + s.size(), std::forward<T>(value)); }

template <typename T> bool fromChars(const string& s, T&& value) {
    return fromChars(s.data(), s.data() + s.size(), std::forward<T>(value));
}

static void handleIpcPacket(const IpcPacket& packet, const shared_ptr<BackgroundImagesLoader>& imagesLoader) {
    switch (packet.type()) {
        case IpcPacket::OpenImage:
        case IpcPacket::OpenImageV2: {
            auto info = packet.interpretAsOpenImage();
            imagesLoader->enqueue(toPath(info.imagePath), ensureUtf8(info.channelSelector), info.grabFocus);
            break;
        }

        case IpcPacket::ReloadImage: {
            while (!imageViewerIsReady) {}
            auto info = packet.interpretAsReloadImage();
            sImageViewer->scheduleToUiThread([&, info] { sImageViewer->reloadImage(ensureUtf8(info.imageName), info.grabFocus); });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CloseImage: {
            while (!imageViewerIsReady) {}
            auto info = packet.interpretAsCloseImage();
            sImageViewer->scheduleToUiThread([&, info] { sImageViewer->removeImage(ensureUtf8(info.imageName)); });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::UpdateImage:
        case IpcPacket::UpdateImageV2:
        case IpcPacket::UpdateImageV3: {
            while (!imageViewerIsReady) {}
            auto info = packet.interpretAsUpdateImage();
            sImageViewer->scheduleToUiThread([&, info] {
                string imageString = ensureUtf8(info.imageName);
                for (int i = 0; i < info.nChannels; ++i) {
                    sImageViewer->updateImage(
                        imageString, info.grabFocus, info.channelNames[i], info.x, info.y, info.width, info.height, info.imageData[i]
                    );
                }
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CreateImage: {
            while (!imageViewerIsReady) {}
            auto info = packet.interpretAsCreateImage();
            sImageViewer->scheduleToUiThread([&, info] {
                stringstream imageStream;
                imageStream << "empty" << " " << info.width << " " << info.height << " " << info.nChannels << " ";
                for (int i = 0; i < info.nChannels; ++i) {
                    // The following lines encode strings by prefixing their length. The reason for using this encoding is to allow
                    // arbitrary characters, including whitespaces, in the channel names.
                    imageStream << info.channelNames[i].length() << info.channelNames[i];
                }

                auto imagesLoadTask = tryLoadImage(
                    toPath(info.imageName),
                    imageStream,
                    "",
                    sImageViewer->imagesLoader().imageLoaderSettings(),
                    sImageViewer->imagesLoader().groupChannels()
                );
                const auto images = imagesLoadTask.get();

                if (!images.empty()) {
                    sImageViewer->replaceImage(ensureUtf8(info.imageName), images.front(), info.grabFocus);
                    TEV_ASSERT(images.size() == 1, "IPC CreateImage should never create more than 1 image at once.");
                }
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::VectorGraphics: {
            while (!imageViewerIsReady) {}
            auto info = packet.interpretAsVectorGraphics();
            sImageViewer->scheduleToUiThread([&, info] {
                sImageViewer->updateImageVectorGraphics(ensureUtf8(info.imageName), info.grabFocus, info.append, info.commands);
            });

            sImageViewer->redraw();
            break;
        }

        default: {
            throw runtime_error{fmt::format("Invalid IPC packet type {}", (int)packet.type())};
        }
    }
}

static void convertTo(
    const string& targetPathPattern,
    const shared_ptr<BackgroundImagesLoader>& imagesLoader,
    EMetric metric,
    const nanogui::Color& bg,
    ETonemap tonemap,
    float gamma,
    float exposure,
    float offset
) {
    const int priority = numeric_limits<int>::max();
    unordered_set<fs::path> writtenPaths;

    const auto start = chrono::steady_clock::now();
    const ScopeGuard guard{[&]() {
        if (writtenPaths.empty()) {
            return;
        }

        const auto elapsedSeconds = chrono::duration<double>{chrono::steady_clock::now() - start}.count();
        tlog::success() << fmt::format("Converted {} images in {:.3f} seconds.", writtenPaths.size(), elapsedSeconds);
    }};

    vector<Task<void>> saveTasks;
    for (size_t idx = 0; const auto imageAddition = imagesLoader->tryPop(); ++idx) {
        if (imageAddition->images.empty()) {
            tlog::error() << fmt::format("Image addition is empty, cannot convert");
            continue;
        }

        // TODO: support saving images with multiple frames (if output format permits). Currently only the first frame is saved.
        const auto& image = imageAddition->images.front();
        if (image->channelGroups().empty()) {
            tlog::error() << fmt::format("Image {} has no channel groups, cannot convert", image->path());
            continue;
        }

        const auto path = toPath(substituteCurly(targetPathPattern, [&](string_view placeholder) {
            const auto parts = split(placeholder, ":");
            const auto fmt = parts[0];
            if (fmt == "file" && parts.size() == 1) {
                return toString(image->path().stem());
            } else if (fmt == "dir" && parts.size() == 1) {
                return toString(image->path().parent_path());
            } else if (fmt == "ext" && parts.size() == 1) {
                return toString(image->path().extension());
            } else if (fmt == "idx" && parts.size() == 1) {
                return fmt::format("{}", idx);
            } else if (fmt == "idx" && parts.size() == 2) {
                return fmt::format(fmt::runtime(fmt::format("{{:{}}}", parts[1])), idx);
            } else {
                throw runtime_error{fmt::format("Invalid placeholder '{{{}}}'", placeholder)};
            }
        }));

        if (writtenPaths.find(path) != writtenPaths.end()) {
            tlog::info() << fmt::format("Skipping conversion of {} to {} as this path was already written to", image->path(), path);
            continue;
        }

        writtenPaths.insert(path);
        if (path == image->path()) {
            tlog::info() << fmt::format("Skipping conversion of {} to itself", image->path());
            continue;
        }

        saveTasks.emplace_back(
            [](shared_ptr<Image> image,
               fs::path path,
               EMetric metric,
               nanogui::Color bg,
               ETonemap tonemap,
               float gamma,
               float exposure,
               float offset,
               int priority) -> Task<void> {
                try {
                    co_await ThreadPool::global().enqueueCoroutine(priority);
                    const auto saveStart = chrono::steady_clock::now();

                    // TODO: support saving images with multiple channel groups (if output format permits). Currently only RGBA.
                    const auto cg = image->channelGroups().front().name;
                    const auto window = image->toImageCoords(image->displayWindow());
                    co_await image->save(path, nullptr, window, cg, metric, bg, tonemap, gamma, exposure, offset, priority);

                    const auto saveElapsedSeconds = chrono::duration<double>{chrono::steady_clock::now() - saveStart}.count();
                    tlog::success() << fmt::format("Converted {} to {} after {:.3f} seconds", image->path(), path, saveElapsedSeconds);
                } catch (const ImageSaveError& e) {
                    tlog::error() << fmt::format("Could not convert {} to {}: {}", image->path(), path, e.what());
                }
            }(image, path, metric, bg, tonemap, gamma, exposure, offset, priority)
        );
    }

    waitAll(saveTasks);
}

static int mainFunc(span<const string> arguments) {
    ArgumentParser parser{
        "tev — The EDR Viewer\n"
        "version " TEV_VERSION
        "\n"
        "Inspection tool for images with high dynamic range",
        "tev was developed by Thomas Müller <contact@tom94.net>. "
        "Its source code is available under the GPLv3 License at https://tom94.net/tev",
    };

    Flag autoFitFlag{
        parser,
        "AUTO FIT",
        "Automatically fit selected images to tev's window size.",
        {"auto-fit"},
    };

    ValueFlag<string> backgroundColorFlag{
        parser,
        "COLOR",
        "The background color to blend images against. "
        "Specify as sRGB hex code (#RGB, #RGBA, #RRGGBB, or #RRGGBBAA) or as linear comma-separated RGB(A) values (e.g. 0.5,0.5,0.5 or 0.5,0.5,0.5,1). "
        "Alpha is straight. "
        "Default is transparent, i.e. #00000000",
        {"bg", "background-color"},
    };

    Flag channelGroupingFlagOff{
        parser,
        "NO CHANNEL GROUPING",
        "Do not group channels into channel groups.",
        {"no-channel-grouping"},
    };

    ValueFlag<string> convertToFlag{
        parser,
        "PATH",
        "Run tev in conversion mode without opening a window. "
        "In this mode, tev will convert all supplied images to the file extension of PATH. "
        "Supported formats are bmp, exr, hdr, jpg, jxl, png, tga. "
        "PATH may contain special placeholders:\n"
        "{file}: the original filename without directory or extension\n"
        "{dir}: the original file's directory\n"
        "{ext}: the original file's extension\n"
        "{idx:format}: the index of the image in the list of supplied images, formatted according to 'format' (e.g. 03 for zero-padded three digits)\n"
        "\nExamples:\n"
        "tev --convert-to {dir}/{file}_converted.png image1.exr image2.hdr\n"
        "tev --convert-to /output/directory/{file}.jpg image1.exr image2.h\n"
        "tev --convert-to /output/directory/converted_{idx:02}.png image1.exr image2.hdr image3.png\n",
        {'c', "convert-to"},
    };

    Flag dngCameraProfileFlag{
        parser,
        "DNG CAMERA PROFILE",
        "When loading DNG images, apply the embedded camera profile. "
        "Enabling this setting moves the image farther from the raw sensor response and closer to a pleasing image, but potentially at the cost of colorimetric accuracy. "
        "Regardless of this setting, the DNG's embedded color space, linearization, and white balance metadata will always be applied. "
        "Default is off.",
        {"dng-camera-profile"},
    };

    ValueFlag<float> exposureFlag{
        parser,
        "EXPOSURE",
        "Scales the brightness of an image prior to tonemapping by 2^EXPOSURE. Default is 0.",
        {'e', "exposure"},
    };

    ValueFlag<string> filterFlag{
        parser,
        "FILTER",
        "Filter visible images and groups according to a supplied string. "
        "The string must have the format 'image:group'. "
        "Only images whose name contains 'image' and groups whose name contains 'group' will be visible.",
        {'f', "filter"},
    };

    ValueFlag<int> fpsFlag{
        parser,
        "FPS",
        "Frames per second during playback",
        {"fps"},
    };

    ValueFlag<string> gainmapHeadroomFlag{
        parser,
        "HEADROOM",
        "Headroom to use when applying gainmaps in stops. I.e. for a given value of HEADROOM, the maximum brightness in the image's native color space after applying gainmaps will "
        "be 2^HEADROOM or the gainmap's maximum headroom, whichever is smaller. Default is 'inf', always resulting in the gainmap's maximum headroom (maximum HDR). "
        "This flag can also be set to a percentage of the the gainmap's maximum headroom (stops), e.g. '0%' to skip applying gainmaps altogether and '100%' to always apply gainmaps fully."
        "Note that for images with HDR->SDR gainmaps (typically JPEG XL) the percentage indicates how much darkening (not brightening) is applied, whereas stops always indicate brightening.",
        {"gainmap-headroom"},
    };

    ValueFlag<float> gammaFlag{
        parser,
        "GAMMA",
        "The exponent used when TONEMAP is 'Gamma'. Default is 2.2.",
        {'g', "gamma"},
    };

    HelpFlag helpFlag{
        parser,
        "HELP",
        "Display this help menu.",
        {'h', "help"},
    };

    Flag hideUiFlag{
        parser,
        "HIDE UI",
        "Hide the UI on startup.",
        {"hide-ui"},
    };

    ValueFlag<string> hostnameFlag{
        parser,
        "HOSTNAME",
        "The hostname to listen on for IPC communication. "
        "tev can have a distinct primary instance for each unique hostname in use. "
        "Default is 127.0.0.1:14158",
        {"host", "hostname"},
    };

    Flag ldrFlag{
        parser,
        "LDR",
        "Force low dynamic range (8-bit) display colors.",
        {"ldr"},
    };

    Flag maximizeFlagOn{
        parser,
        "MAXIMIZE",
        "Maximize the window on startup. Default is no maximize.",
        {"max", "maximize"},
    };
    Flag maximizeFlagOff{
        parser,
        "NO MAXIMIZE",
        "Do not maximize the window on startup. Default is no maximize.",
        {"no-max", "no-maximize"},
    };

    ValueFlag<string> metricFlag{
        parser,
        "METRIC",
        "The metric to use when comparing two images. "
        "The available metrics are:\n"
        "E: Error\n"
        "AE: Absolute Error\n"
        "SE: Squared Error\n"
        "RAE: Relative Absolute Error\n"
        "RSE: Relative Squared Error\n"
        "Default is E.",
        {'m', "metric"},
    };

    ValueFlag<string> minFilterFlag{
        parser,
        "MIN FILTER",
        "The filter to use when downsampling (minifying) images.",
        {"min-filter"},
    };
    ValueFlag<string> magFilterFlag{
        parser,
        "MAG FILTER",
        "The filter to use when upsampling (magnifying) images.",
        {"mag-filter"},
    };

    Flag newWindowFlagOn{
        parser,
        "NEW WINDOW",
        "Open a new window of tev, even if one exists already. (Default if no images are supplied.)",
        {'n', "new"},
    };
    Flag newWindowFlagOff{
        parser,
        "NO NEW WINDOW",
        "Do not open a new window if one already exists. (Default if images are supplied.)",
        {"no-new"},
    };

    ValueFlag<float> offsetFlag{
        parser,
        "OFFSET",
        "Add an absolute offset to the image after EXPOSURE has been applied. Default is 0.",
        {'o', "offset"},
    };

    Flag playFlag{
        parser,
        "PLAY",
        "Play back images as a video.",
        {'p', "play"},
    };

    ValueFlag<string> sizeFlag{
        parser,
        "SIZE",
        "Initial size of the tev window as <width>x<height>. "
        "Default is 1024x800.",
        {"size"},
    };

    ValueFlag<string> tonemapFlag{
        parser,
        "TONEMAP",
        "The tonemap to use. Available options are:\n"
        "None: No tonemapping\n"
        "Gamma: Gamma curve + inv. sRGB\n"
        "       Needed when rendering SDR to\n"
        "       gamma-encoded displays.\n"
        "FC: False Color\n"
        "PN: Positive=Green, Negative=Red\n"
        "Default is None.",
        {'t', "tonemap"},
    };

    Flag recursiveFlag{
        parser,
        "RECURSIVE",
        "Recursively traverse directories when loading images from them.",
        {'r', "recursive"},
    };

    Flag resizeWindowToFitFlagOn{
        parser,
        "RESIZE TO FIT",
        "Resize the window to fit the image(s) on startup. Default is to resize.",
        {"resize-window"},
    };
    Flag resizeWindowToFitFlagOff{
        parser,
        "NO RESIZE TO FIT",
        "Do not resize the window to fit the image(s) on startup. Default is to resize.",
        {"no-resize-window"},
    };

    Flag verboseFlag{
        parser,
        "VERBOSE",
        "Verbose log output.",
        {'v', "verbose"},
    };

    Flag versionFlag{
        parser,
        "VERSION",
        "Display the version of tev.",
        {'v', "version"},
    };

    Flag watchFlag{
        parser,
        "WATCH",
        "Watch image files and directories for changes and automatically reload them.",
        {'w', "watch"},
    };

    ValueFlag<string> whiteLevelFlag{
        parser,
        "WHITE LEVEL",
        "Override the system's display white level in nits (cd/m²). "
        "Also known as \"reference white\" or \"paper white\". "
        "Only possible on HDR systems with absolute brightness capability. "
        "You can also set the white level to 'image' to use the image's metadata white level if available.",
        {"wl", "white-level"},
    };

    PositionalList<string> imageFiles{
        parser,
        "images",
        "The image files to be opened by tev. "
        "If an argument starting with a ':' is encountered, "
        "then this argument is not treated as an image file "
        "but as a comma-separated channel selector. Until the next channel "
        "selector is encountered only channels containing "
        "elements from the current selector will be loaded. This is "
        "especially useful for selectively loading a specific "
        "part of a multi-part EXR file.",
    };

    // Parse command line arguments and react to parsing errors using exceptions.
    TEV_ASSERT(arguments.size() > 0, "Number of arguments must be bigger than 0.");

    try {
        parser.Prog(arguments.front());
        parser.ParseArgs(begin(arguments) + 1, end(arguments));
    } catch (const Help&) {
        cout << parser;
        return 0;
    } catch (const ParseError& e) {
        cerr << fmt::format("{}\nUsage: {} --help\n", e.what(), arguments.front());
        return -1;
    } catch (const ValidationError& e) {
        cerr << fmt::format("{}\nUsage: {} --help\n", e.what(), arguments.front());
        return -2;
    }

    if (verboseFlag) {
        tlog::Logger::global()->showSeverity(tlog::ESeverity::Debug);
    }

    if (versionFlag) {
        tlog::none() << "tev — The EDR Viewer\nversion " TEV_VERSION;
        return 0;
    }

    if (newWindowFlagOn && newWindowFlagOff) {
        tlog::error() << "Ambiguous '--new' arguments.";
        return -3;
    }

    // If we don't have any images to load, create new windows regardless of flag. (In this case, the user likely wants to open a new
    // instance of tev rather than focusing the existing one.)
    const bool newWindow = (!imageFiles && !newWindowFlagOff) || newWindowFlagOn;

    const auto ipc = convertToFlag ? nullptr : (hostnameFlag ? make_shared<Ipc>(get(hostnameFlag)) : make_shared<Ipc>());

    // If we're not the primary instance and did not request to open a new window, simply send the to-be-opened images to the primary
    // instance.
    if (ipc && !ipc->isPrimaryInstance() && !newWindow) {
        string channelSelector;
        bool first = true;
        for (auto imageFile : get(imageFiles)) {
            if (!imageFile.empty() && imageFile[0] == ':') {
                channelSelector = imageFile.substr(1);
                continue;
            }

            fs::path imagePath = toPath(imageFile);
            if (!fs::exists(imagePath)) {
                tlog::error() << fmt::format("Image {} does not exist.", imagePath);
                continue;
            }

            try {
                IpcPacket packet;
                packet.setOpenImage(
                    toString(fs::canonical(imagePath)),
                    channelSelector,
                    first // select the first image among those that are loaded
                );
                first = false;

                ipc->sendToPrimaryInstance(packet);
            } catch (const runtime_error& e) { tlog::error() << fmt::format("Unexpected error {}: {}", imagePath, e.what()); }
        }

        return 0;
    }

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    const shared_ptr<BackgroundImagesLoader> imagesLoader = make_shared<BackgroundImagesLoader>();
    imagesLoader->setRecursiveDirectories(recursiveFlag);
    imagesLoader->setGroupChannels(!channelGroupingFlagOff);

    if (dngCameraProfileFlag) {
        imagesLoader->imageLoaderSettings().dngApplyCameraProfile = true;
    }

    if (gainmapHeadroomFlag) {
        try {
            const auto gainmapHeadroom = GainmapHeadroom{get(gainmapHeadroomFlag)};
            imagesLoader->imageLoaderSettings().gainmapHeadroom = gainmapHeadroom;
        } catch (const invalid_argument& e) {
            tlog::error() << fmt::format("Invalid gainmap headroom '{}': {}", get(gainmapHeadroomFlag), e.what());
            return -6;
        }
    }

    // Spawn a background thread that opens images passed via stdin. To allow whitespace characters in filenames, we use the convention that
    // paths in stdin must be separated by newlines.
    auto stdinThread = thread{[weakImagesLoader = weak_ptr<BackgroundImagesLoader>{imagesLoader}]() {
        string channelSelector;
        while (!shuttingDown()) {
            for (string line; getline(cin, line) && !shuttingDown();) {
                string imageFile = tev::ensureUtf8(line);

                if (imageFile.empty()) {
                    continue;
                }

                if (imageFile[0] == ':') {
                    channelSelector = imageFile.substr(1);
                    continue;
                }

                if (auto imagesLoader = weakImagesLoader.lock(); imagesLoader) {
                    imagesLoader->enqueue(tev::toPath(imageFile), channelSelector, false);
                }
            }

            this_thread::sleep_for(10ms);
        }
    }};

    // HACK: It is unfortunately not easily possible to poll/timeout on cin in a portable manner, so instead we resort to simply detaching
    // this thread, causing it to be forcefully terminated as the main thread terminates. Also, on some Linux systems, this will still not
    // terminate, so we schedule exit(0) to be called as well.
    stdinThread.detach();

    // Spawn another background thread, this one dealing with images passed to us via inter-process communication (IPC). This happens when a
    // user starts another instance of tev while one is already running. Note, that this behavior can be overridden by the -n flag, so not
    // _all_ secondary instances send their paths to the primary instance.
    auto ipcThread = !ipc ? thread{} : thread{[&]() {
        try {
            while (!shuttingDown()) {
                // Attempt to become primary instance in case the primary instance got closed at some point. Attempt this with a reasonably
                // low frequency to not hog CPU/OS resources.
                if (!ipc->isPrimaryInstance() && !ipc->attemptToBecomePrimaryInstance()) {
                    this_thread::sleep_for(100ms);
                    continue;
                }

                ipc->receiveFromSecondaryInstance([&](const IpcPacket& packet) {
                    try {
                        handleIpcPacket(packet, imagesLoader);
                    } catch (const runtime_error& e) { tlog::warning() << "Malformed IPC packet: " << e.what(); }
                });

                this_thread::sleep_for(10ms);
            }
        } catch (const runtime_error& e) { tlog::warning() << "Uncaught exception in IPC thread: " << e.what(); }
    }};

    const ScopeGuard backgroundThreadShutdownGuard{[&]() {
        setShuttingDown();

        ThreadPool::global().waitUntilFinished();
        ThreadPool::global().shutdown();

        if (ipcThread.joinable()) {
            ipcThread.join();
        }

        // stdinThread should not be joinable, since it has been detached earlier. But better to be safe than sorry.
        if (stdinThread.joinable()) {
            stdinThread.join();
        }
    }};

    // Load images passed via command line in the background prior to creating our main application such that they are not stalled by the
    // potentially slow initialization of opengl / glfw.
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        imagesLoader->enqueue(toPath(imageFile), channelSelector, false);
    }

    if (convertToFlag) {
        tlog::info() << "Running in conversion mode. No window will be opened.";

        while (imagesLoader->hasPendingLoads()) {
            this_thread::sleep_for(1ms);
        }

        const EMetric metric = metricFlag ? toMetric(get(metricFlag)) : EMetric::Error;
        const nanogui::Color bg = backgroundColorFlag ? parseColor(get(backgroundColorFlag)) : nanogui::Color{0, 0, 0, 0};
        const ETonemap tonemap = tonemapFlag ? toTonemap(get(tonemapFlag)) : ETonemap::None;
        const float gamma = gammaFlag ? get(gammaFlag) : 2.2f;
        const float exposure = exposureFlag ? get(exposureFlag) : 0.0f;
        const float offset = offsetFlag ? get(offsetFlag) : 0.0f;

        convertTo(get(convertToFlag), imagesLoader, metric, bg, tonemap, gamma, exposure, offset);

        return 0;
    }

    // Init nanogui application
    nanogui::init(!get(ldrFlag));

    const ScopeGuard nanoguiShutdownGuard{[&]() {
    // On some linux distributions glfwTerminate() (which is called by nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
#if defined(__APPLE__) or defined(_WIN32)
        nanogui::shutdown();
#endif
    }};

#ifdef __APPLE__
    // On macOS, the mechanism for opening an application passes filenames through the NS api rather than CLI arguments, which means we need
    // special handling of these through GLFW. There are two components to this special handling:

    // 1. The filenames that were passed to this application when it was opened.
    if (!imageFiles) {
        // If we didn't get any command line arguments for files to open, then, on macOS, they might have been supplied through the NS api.
        const char* const * openedFiles = glfwGetOpenedFilenames();
        if (openedFiles) {
            for (auto p = openedFiles; *p; ++p) {
                imagesLoader->enqueue(toPath(*p), "", false);
            }
        }
    }

    // 2. a callback for when the same application is opened additional times with more files.
    glfwSetOpenedFilenamesCallback([](const char* imageFile) { sImageViewer->imagesLoader().enqueue(toPath(imageFile), "", false); });
#endif

    if (maximizeFlagOn && maximizeFlagOff) {
        tlog::error() << "Ambiguous '--maximize' arguments.";
        return -3;
    }

    // Default false (off-flag is no-op)
    const bool maximize = (false && !maximizeFlagOff) || maximizeFlagOn;

    if (resizeWindowToFitFlagOn && resizeWindowToFitFlagOff) {
        tlog::error() << "Ambiguous '--resize-window' arguments.";
        return -3;
    }

    // Default true (on-flag is no-op)
    const bool resizeWindowToFit = (true && !resizeWindowToFitFlagOff) || resizeWindowToFitFlagOn;

    nanogui::Vector2i size = {1024, 800};
    if (sizeFlag) {
        const string sizeString = get(sizeFlag);
        const auto parts = split(sizeString, "x");
        if (parts.size() != 2) {
            tlog::error() << fmt::format("Invalid size specification '{}'. Must be of the form <width>x<height>.", sizeString);
            return -4;
        }

        if (!fromChars(parts[0], size.x()) || !fromChars(parts[1], size.y())) {
            tlog::error() << fmt::format("Invalid size specification '{}'. Must be of the form <width>x<height>.", sizeString);
            return -4;
        }

        if (size.x() <= 0 || size.y() <= 0) {
            tlog::error() << fmt::format("Invalid size specification '{}'. Width and height must be positive.", sizeString);
            return -4;
        }
    }

    if (!maximize && resizeWindowToFit) {
        // Wait until the first image is loaded before creating the window such that it can size itself appropriately. We can not pass the
        // Window a size right away, because we don't have information about the user's monitor size or DPI scaling yet, hence `size` stays
        // unmodified. However waiting for the first image to load allows `ImageViewer` to size itself to the first image's size early
        // enough that the user will not perceive flickering.
        while (imagesLoader->hasPendingLoads()) {
            if (imagesLoader->firstImageSize()) {
                break;
            }

            this_thread::sleep_for(1ms);
        }
    }

    // sImageViewer is a raw pointer to make sure it will never get deleted. nanogui crashes upon cleanup, so we better not try.
    sImageViewer = new ImageViewer{size, imagesLoader, ipc, maximize, !hideUiFlag, !get(ldrFlag)};
    imageViewerIsReady = true;

    // Apply parameter flags
    if (autoFitFlag) {
        sImageViewer->setAutoFitToScreen(true);
    }

    if (backgroundColorFlag) {
        sImageViewer->setBackgroundColorStraight(parseColor(get(backgroundColorFlag)));
    }

    if (exposureFlag) {
        sImageViewer->setExposure(get(exposureFlag));
    }

    if (filterFlag) {
        sImageViewer->setFilter(get(filterFlag));
    }

    if (fpsFlag) {
        sImageViewer->setFps(get(fpsFlag));
    }

    if (gammaFlag) {
        sImageViewer->setGamma(get(gammaFlag));
    }

    if (metricFlag) {
        sImageViewer->setMetric(toMetric(get(metricFlag)));
    }

    if (minFilterFlag) {
        sImageViewer->setMinFilter(toInterpolationMode(get(minFilterFlag)));
    }

    if (magFilterFlag) {
        sImageViewer->setMagFilter(toInterpolationMode(get(magFilterFlag)));
    }

    if (offsetFlag) {
        sImageViewer->setOffset(get(offsetFlag));
    }

    if (playFlag) {
        sImageViewer->setPlayingBack(true);
    }

    if (resizeWindowToFitFlagOn || resizeWindowToFitFlagOff) {
        sImageViewer->setResizeWindowToFitImageOnLoad(resizeWindowToFit);
    }

    if (tonemapFlag) {
        sImageViewer->setTonemap(toTonemap(get(tonemapFlag)));
    }

    if (watchFlag) {
        sImageViewer->setWatchFilesForChanges(true);
    }

    if (whiteLevelFlag) {
        const string wlValue = get(whiteLevelFlag);
        if (toLower(wlValue) == "image") {
            sImageViewer->setDisplayWhiteLevelSetting(ImageViewer::EDisplayWhiteLevelSetting::ImageMetadata);
        } else {
            try {
                const float whiteLevel = stof(get(whiteLevelFlag));
                sImageViewer->setDisplayWhiteLevelSetting(ImageViewer::EDisplayWhiteLevelSetting::Custom);
                sImageViewer->setDisplayWhiteLevel(whiteLevel);
            } catch (const invalid_argument&) {
                tlog::error() << fmt::format("Invalid white level value '{}'. Must be a float or 'image'.", get(whiteLevelFlag));
                return -5;
            }
        }
    }

    sImageViewer->draw_all();
    sImageViewer->set_visible(true);
    sImageViewer->redraw();

    // Refresh only every 250ms if there are no user interactions. This makes an idling tev surprisingly energy-efficient. :)
    nanogui::run(nanogui::RunMode::Lazy);

    return 0;
}

} // namespace tev

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif
    try {
        // This accelerates I/O significantly by allowing C++ to perform its own buffering. Furthermore, this prevents a failure to
        // forcefully close the stdin thread in case of a shutdown on certain Linux systems.
        ios::sync_with_stdio(false);

        vector<string> arguments;
        for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
            arguments.emplace_back(tev::utf16to8(argv[i]));
#else
            string arg = argv[i];
            // OSX sometimes (seemingly sporadically) passes the process serial number via a command line parameter. We would like to ignore
            // this.
            if (arg.find("-psn") != 0) {
                arguments.emplace_back(tev::ensureUtf8(argv[i]));
            }
#endif
        }

        return tev::mainFunc(arguments);
    } catch (const exception& e) {
        tlog::error() << fmt::format("Uncaught exception: {}", e.what());
        return 1;
    }
}
