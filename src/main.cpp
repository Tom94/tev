// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/ImageViewer.h>
#include <tev/Ipc.h>
#include <tev/ThreadPool.h>

#include <args.hxx>
#include <ImfThreading.h>

#include <utf8.h>

#ifdef __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include <chrono>
#include <iostream>
#include <thread>

using namespace args;
using namespace std;

TEV_NAMESPACE_BEGIN

// Image viewer is a static variable to allow other
// parts of the program to easily schedule operations
// onto the main nanogui thread loop.
// In a truly modular program, this would never be required,
// but OpenGL's state-machine nature throws a wrench into
// modularity.
// Currently, the only use case is the destruction of
// OpenGL textures, which _must_ happen on the thread
// on which the GL context is "current".
static ImageViewer* sImageViewer = nullptr;
static atomic<bool> imageViewerIsReady = false;

void scheduleToMainThread(const std::function<void()>& fun) {
    if (imageViewerIsReady) {
        sImageViewer->scheduleToUiThread(fun);
    }
}

void redrawWindow() {
    if (imageViewerIsReady) {
        sImageViewer->redraw();
    }
}

void handleIpcPacket(const IpcPacket& packet, const std::shared_ptr<BackgroundImagesLoader>& imagesLoader) {
    switch (packet.type()) {
        case IpcPacket::OpenImage:
        case IpcPacket::OpenImageV2: {
            auto info = packet.interpretAsOpenImage();
            imagesLoader->enqueue(toPath(info.imagePath), ensureUtf8(info.channelSelector), info.grabFocus);
            break;
        }

        case IpcPacket::ReloadImage: {
            while (!imageViewerIsReady) { }
            auto info = packet.interpretAsReloadImage();
            sImageViewer->scheduleToUiThread([&, info] {
                sImageViewer->reloadImage(ensureUtf8(info.imageName), info.grabFocus);
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CloseImage: {
            while (!imageViewerIsReady) { }
            auto info = packet.interpretAsCloseImage();
            sImageViewer->scheduleToUiThread([&, info] {
                sImageViewer->removeImage(ensureUtf8(info.imageName));
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::UpdateImage:
        case IpcPacket::UpdateImageV2:
        case IpcPacket::UpdateImageV3: {
            while (!imageViewerIsReady) { }
            auto info = packet.interpretAsUpdateImage();
            sImageViewer->scheduleToUiThread([&, info] {
                string imageString = ensureUtf8(info.imageName);
                for (int i = 0; i < info.nChannels; ++i) {
                    sImageViewer->updateImage(imageString, info.grabFocus, info.channelNames[i], info.x, info.y, info.width, info.height, info.imageData[i]);
                }
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CreateImage: {
            while (!imageViewerIsReady) { }
            auto info = packet.interpretAsCreateImage();
            sImageViewer->scheduleToUiThread([&, info] {
                stringstream imageStream;
                imageStream
                    << "empty" << " "
                    << info.width << " "
                    << info.height << " "
                    << info.nChannels << " "
                    ;
                for (int i = 0; i < info.nChannels; ++i) {
                    // The following lines encode strings by prefixing their length.
                    // The reason for using this encoding is to allow  arbitrary characters,
                    // including whitespaces, in the channel names.
                    imageStream << info.channelNames[i].length() << info.channelNames[i];
                }

                auto images = tryLoadImage(toPath(info.imageName), imageStream, "").get();
                if (!images.empty()) {
                    sImageViewer->replaceImage(ensureUtf8(info.imageName), images.front(), info.grabFocus);
                    TEV_ASSERT(images.size() == 1, "IPC CreateImage should never create more than 1 image at once.");
                }
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::VectorGraphics: {
            while (!imageViewerIsReady) { }
            auto info = packet.interpretAsVectorGraphics();
            sImageViewer->scheduleToUiThread([&, info] {
                sImageViewer->updateImageVectorGraphics(ensureUtf8(info.imageName), info.grabFocus, info.append, info.commands);
            });

            sImageViewer->redraw();
            break;
        }

        default: {
            throw runtime_error{format("Invalid IPC packet type {}", (int)packet.type())};
        }
    }
}

int mainFunc(const vector<string>& arguments) {
    ArgumentParser parser{
        "tev — The EXR Viewer\n"
        "version " TEV_VERSION "\n"
        "Inspection tool for images with high dynamic range",
        "tev was developed by Thomas Müller <thomas94@gmx.net>. "
        "Its source code is available under the BSD 3-Clause License at https://tom94.net",
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

    Flag maximizeFlagOn{parser, "MAXIMIZE", "Maximize the window on startup. (Default if images are supplied.)", {"max", "maximize"}};
    Flag maximizeFlagOff{parser, "NO MAXIMIZE", "Do not maximize the window on startup. (Default if no images are supplied.)", {"no-max", "no-maximize"}};

    ValueFlag<string> metricFlag{
        parser,
        "METRIC",
        "The metric to use when comparing two images. "
        "The available metrics are:\n"
        "E   - Error\n"
        "AE  - Absolute Error\n"
        "SE  - Squared Error\n"
        "RAE - Relative Absolute Error\n"
        "RSE - Relative Squared Error\n"
        "Default is E.",
        {'m', "metric"},
    };

    Flag newWindowFlagOn{parser, "NEW WINDOW", "Open a new window of tev, even if one exists already. (Default if no images are supplied.)", {'n', "new"}};
    Flag newWindowFlagOff{parser, "NO NEW WINDOW", "Do not open a new window if one already exists. (Default if images are supplied.)", {"no-new"}};

    ValueFlag<float> offsetFlag{
        parser,
        "OFFSET",
        "Add an absolute offset to the image after EXPOSURE has been applied. Default is 0.",
        {'o', "offset"},
    };

    ValueFlag<string> tonemapFlag{
        parser,
        "TONEMAP",
        "The tonemapping algorithm to use. "
        "The available tonemaps are:\n"
        "sRGB   - sRGB\n"
        "Gamma  - Gamma curve\n"
        "FC     - False Color\n"
        "PN     - Positive=Green, Negative=Red\n"
        "Default is sRGB.",
        {'t', "tonemap"},
    };

    Flag recursiveFlag{
        parser,
        "RECURSIVE",
        "Recursively traverse directories when loading images from them.",
        {'r', "recursive"},
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

    // Parse command line arguments and react to parsing
    // errors using exceptions.
    try {
        TEV_ASSERT(arguments.size() > 0, "Number of arguments must be bigger than 0.");

        parser.Prog(arguments.front());
        parser.ParseArgs(begin(arguments) + 1, end(arguments));
    } catch (const Help&) {
        cout << parser;
        return 0;
    } catch (const ParseError& e) {
        cerr << e.what() << endl;
        return -1;
    } catch (const ValidationError& e) {
        cerr << e.what() << endl;
        return -2;
    }

    if (versionFlag) {
        tlog::none() << "tev — The EXR Viewer\nversion " TEV_VERSION;
        return 0;
    }

    auto ipc = hostnameFlag ? make_shared<Ipc>(get(hostnameFlag)) : make_shared<Ipc>();

    // If we don't have any images to load, create new windows regardless of flag.
    // (In this case, the user likely wants to open a new instance of tev rather
    // than focusing the existing one.)
    bool newWindow = !imageFiles;
    if (newWindowFlagOn) { newWindow = true; }
    if (newWindowFlagOff) { newWindow = false; }

    if (newWindowFlagOn && newWindowFlagOff) {
        tlog::error() << "Ambiguous 'new window' arguments.";
        return -3;
    }

    // If we're not the primary instance and did not request to open a new window,
    // simply send the to-be-opened images to the primary instance.
    if (!ipc->isPrimaryInstance() && !newWindow) {
        string channelSelector;
        bool first = true;
        for (auto imageFile : get(imageFiles)) {
            if (!imageFile.empty() && imageFile[0] == ':') {
                channelSelector = imageFile.substr(1);
                continue;
            }

            fs::path imagePath = toPath(imageFile);
            if (!fs::exists(imagePath)) {
                tlog::error() << format("Image {} does not exist.", imagePath);
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
            } catch (const runtime_error& e) {
                tlog::error() << format("Unexpected error {}: {}", imagePath, e.what());
            }
        }

        return 0;
    }

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    shared_ptr<BackgroundImagesLoader> imagesLoader = make_shared<BackgroundImagesLoader>();
    if (recursiveFlag) {
        imagesLoader->setRecursiveDirectories(true);
    }

    // Spawn a background thread that opens images passed via stdin.
    // To allow whitespace characters in filenames, we use the convention that
    // paths in stdin must be separated by newlines.
    thread stdinThread{[&]() {
        string channelSelector;
        while (!shuttingDown()) {
            for (string line; getline(cin, line);) {
                string imageFile = tev::ensureUtf8(line);

                if (imageFile.empty()) {
                    continue;
                }

                if (imageFile[0] == ':') {
                    channelSelector = imageFile.substr(1);
                    continue;
                }

                imagesLoader->enqueue(tev::toPath(imageFile), channelSelector, false);
            }

            this_thread::sleep_for(100ms);
        }
    }};

    // It is unfortunately not easily possible to poll/timeout on cin in a portable manner,
    // so instead we resort to simply detaching this thread, causing it to be forcefully
    // terminated as the main thread terminates.
    stdinThread.detach();

    // Spawn another background thread, this one dealing with images passed to us
    // via inter-process communication (IPC). This happens when
    // a user starts another instance of tev while one is already running. Note, that this
    // behavior can be overridden by the -n flag, so not _all_ secondary instances send their
    // paths to the primary instance.
    thread ipcThread = thread{[&]() {
        try {
            while (!shuttingDown()) {
                // Attempt to become primary instance in case the primary instance
                // got closed at some point. Attempt this with a reasonably low frequency
                // to not hog CPU/OS resources.
                if (!ipc->isPrimaryInstance() && !ipc->attemptToBecomePrimaryInstance()) {
                    this_thread::sleep_for(100ms);
                    continue;
                }

                ipc->receiveFromSecondaryInstance([&](const IpcPacket& packet) {
                    try {
                        handleIpcPacket(packet, imagesLoader);
                    } catch (const runtime_error& e) {
                        tlog::warning() << "Malformed IPC packet: " << e.what();
                    }
                });

                this_thread::sleep_for(10ms);
            }
        } catch (const runtime_error& e) {
            tlog::warning() << "Uncaught exception in IPC thread: " << e.what();
        }
    }};

    ScopeGuard backgroundThreadShutdownGuard{[&]() {
        setShuttingDown();

        if (ipcThread.joinable()) {
            ipcThread.join();
        }

        // stdinThread should not be joinable, since it has been
        // detached earlier. But better to be safe than sorry.
        if (stdinThread.joinable()) {
            stdinThread.join();
        }
    }};

    // Load images passed via command line in the background prior to
    // creating our main application such that they are not stalled
    // by the potentially slow initialization of opengl / glfw.
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        imagesLoader->enqueue(toPath(imageFile), channelSelector, false);
    }

    // Init nanogui application
    nanogui::init();

    ScopeGuard nanoguiShutdownGuard{[&]() {
        // On some linux distributions glfwTerminate() (which is called by
        // nanogui::shutdown()) causes segfaults. Since we are done with our
        // program here anyways, let's let the OS clean up after us.
#if defined(__APPLE__) or defined(_WIN32)
        nanogui::shutdown();
#endif
    }};

#ifdef __APPLE__
    // On macOS, the mechanism for opening an application passes filenames
    // through the NS api rather than CLI arguments, which means we need
    // special handling of these through GLFW.
    // There are two components to this special handling:

    // 1. The filenames that were passed to this application when it was opened.
    if (!imageFiles) {
        // If we didn't get any command line arguments for files to open,
        // then, on macOS, they might have been supplied through the NS api.
        const char* const* openedFiles = glfwGetOpenedFilenames();
        if (openedFiles) {
            for (auto p = openedFiles; *p; ++p) {
                imagesLoader->enqueue(toPath(*p), "", false);
            }
        }
    }

    // 2. a callback for when the same application is opened additional
    //    times with more files.
    glfwSetOpenedFilenamesCallback([](const char* imageFile) {
        sImageViewer->imagesLoader().enqueue(toPath(imageFile), "", false);
    });
#endif

    auto [capability10bit, capabilityEdr] = nanogui::test_10bit_edr_support();
    if (get(ldrFlag)) {
        capability10bit = false;
        capabilityEdr = false;
    }

    tlog::info() << "Launching with " << (capability10bit ? 10 : 8) << " bits of color and " << (capabilityEdr ? "HDR" : "LDR") << " display support.";

    // Do what the maximize flag tells us---if it exists---and
    // maximize if we have images otherwise.
    bool maximize = imageFiles;
    if (maximizeFlagOn) { maximize = true; }
    if (maximizeFlagOff) { maximize = false; }

    if (maximizeFlagOn && maximizeFlagOff) {
        tlog::error() << "Ambiguous 'maximize' arguments.";
        return -3;
    }

    // sImageViewer is a raw pointer to make sure it will never
    // get deleted. nanogui crashes upon cleanup, so we better
    // not try.
    sImageViewer = new ImageViewer{imagesLoader, maximize, capability10bit || capabilityEdr, capabilityEdr};
    imageViewerIsReady = true;

    sImageViewer->draw_all();
    sImageViewer->set_visible(true);
    sImageViewer->redraw();

    // Apply parameter flags
    if (exposureFlag) { sImageViewer->setExposure(get(exposureFlag)); }
    if (filterFlag)   { sImageViewer->setFilter(get(filterFlag)); }
    if (gammaFlag)    { sImageViewer->setGamma(get(gammaFlag)); }
    if (metricFlag)   { sImageViewer->setMetric(toMetric(get(metricFlag))); }
    if (offsetFlag)   { sImageViewer->setOffset(get(offsetFlag)); }
    if (tonemapFlag)  { sImageViewer->setTonemap(toTonemap(get(tonemapFlag))); }
    if (watchFlag)    { sImageViewer->setWatchFilesForChanges(true); }

    // Refresh only every 250ms if there are no user interactions.
    // This makes an idling tev surprisingly energy-efficient. :)
    nanogui::mainloop(250);

    return 0;
}

TEV_NAMESPACE_END

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif
    try {
        vector<string> arguments;
        for (int i = 0; i < argc; ++i) {
#ifdef _WIN32
            arguments.emplace_back(tev::utf16to8(argv[i]));
#else
            string arg = argv[i];
            // OSX sometimes (seemingly sporadically) passes the
            // process serial number via a command line parameter.
            // We would like to ignore this.
            if (arg.find("-psn") != 0) {
                arguments.emplace_back(tev::ensureUtf8(argv[i]));
            }
#endif
        }

        return tev::mainFunc(arguments);
    } catch (const exception& e) {
        tlog::error() << format("Uncaught exception: {}", e.what());
        return 1;
    }
}
