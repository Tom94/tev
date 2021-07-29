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

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace args;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

ThreadPool* gThreadPool = new ThreadPool{};

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

void scheduleToMainThread(const std::function<void()>& fun) {
    if (sImageViewer) {
        sImageViewer->scheduleToUiThread(fun);
    }
}

void handleIpcPacket(const IpcPacket& packet, const std::shared_ptr<BackgroundImagesLoader>& imagesLoader) {
    switch (packet.type()) {
        case IpcPacket::OpenImage:
        case IpcPacket::OpenImageV2: {
            auto info = packet.interpretAsOpenImage();
            imagesLoader->enqueue(ensureUtf8(info.imagePath), ensureUtf8(info.channelSelector), info.grabFocus);
            break;
        }

        case IpcPacket::ReloadImage: {
            while (!sImageViewer) { }
            auto info = packet.interpretAsReloadImage();
            sImageViewer->scheduleToUiThread([&,info] {
                string imageString = ensureUtf8(info.imageName);
                sImageViewer->reloadImage(imageString, info.grabFocus);
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CloseImage: {
            while (!sImageViewer) { }
            auto info = packet.interpretAsCloseImage();
            sImageViewer->scheduleToUiThread([&,info] {
                string imageString = ensureUtf8(info.imageName);
                sImageViewer->removeImage(imageString);
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::UpdateImage:
        case IpcPacket::UpdateImageV2:
        case IpcPacket::UpdateImageV3: {
            while (!sImageViewer) { }
            auto info = packet.interpretAsUpdateImage();
            sImageViewer->scheduleToUiThread([&,info] {
                string imageString = ensureUtf8(info.imageName);
                for (int i = 0; i < info.nChannels; ++i) {
                    sImageViewer->updateImage(imageString, info.grabFocus, info.channelNames[i], info.x, info.y, info.width, info.height, info.imageData[i]);
                }
            });

            sImageViewer->redraw();
            break;
        }

        case IpcPacket::CreateImage: {
            while (!sImageViewer) { }
            auto info = packet.interpretAsCreateImage();
            sImageViewer->scheduleToUiThread([&,info] {
                string imageString = ensureUtf8(info.imageName);
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

                auto image = tryLoadImage(imageString, imageStream, "");
                if (image) {
                    sImageViewer->addImage(image, info.grabFocus);
                }
            });

            sImageViewer->redraw();
            break;
        }

        default: {
            throw runtime_error{tfm::format("Invalid IPC packet type %d", (int)packet.type())};
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

    ValueFlag<bool> maximizeFlag{
        parser,
        "MAXIMIZE",
        "Maximize the window on startup. "
        "If no images were supplied via the command line, then the default is FALSE. "
        "Otherwise, the default is TRUE.",
        {"max", "maximize"},
    };

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

    Flag newWindowFlag{
        parser,
        "NEW WINDOW",
        "Open a new window of tev, even if one exists already.",
        {'n', "new"},
    };

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

    Flag versionFlag{
        parser,
        "VERSION",
        "Display the version of tev.",
        {'v', "version"},
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
        cerr << parser;
        return -1;
    } catch (const ValidationError& e) {
        cerr << e.what() << endl;
        cerr << parser;
        return -2;
    }

    if (versionFlag) {
        tlog::none() << "tev — The EXR Viewer\nversion " TEV_VERSION;
        return 0;
    }

    const string hostname = hostnameFlag ? get(hostnameFlag) : "127.0.0.1:14158";
    auto ipc = make_shared<Ipc>(hostname);

    // If we're not the primary instance and did not request to open a new window,
    // simply send the to-be-opened images to the primary instance.
    if (!ipc->isPrimaryInstance() && !newWindowFlag) {
        string channelSelector;
        for (auto imageFile : get(imageFiles)) {
            if (!imageFile.empty() && imageFile[0] == ':') {
                channelSelector = imageFile.substr(1);
                continue;
            }

            try {
                IpcPacket packet;
                packet.setOpenImage(path{imageFile}.make_absolute().str(), channelSelector, true);
                ipc->sendToPrimaryInstance(packet);
            } catch (const runtime_error& e) {
                tlog::error() << tfm::format("Invalid file '%s': %s", imageFile, e.what());
            }
        }

        return 0;
    }

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    tlog::info() << "Loading window...";

    shared_ptr<BackgroundImagesLoader> imagesLoader = make_shared<BackgroundImagesLoader>();

    atomic<bool> shallShutdown{false};

    // Spawn a background thread that opens images passed via stdin.
    // To allow whitespace characters in filenames, we use the convention that
    // paths in stdin must be separated by newlines.
    thread stdinThread{[&]() {
        string channelSelector;
        while (!shallShutdown) {
            for (string line; getline(cin, line);) {
                string imageFile = tev::ensureUtf8(line);

                if (imageFile.empty()) {
                    continue;
                }

                if (imageFile[0] == ':') {
                    channelSelector = imageFile.substr(1);
                    continue;
                }

                imagesLoader->enqueue(imageFile, channelSelector, false);
            }

            this_thread::sleep_for(chrono::milliseconds{100});
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
    thread ipcThread;
    if (ipc->isPrimaryInstance()) {
        ipcThread = thread{[&]() {
            while (!shallShutdown) {
                ipc->receiveFromSecondaryInstance([&](const IpcPacket& packet) {
                    try {
                        handleIpcPacket(packet, imagesLoader);
                    } catch (const runtime_error& e) {
                        tlog::warning() << "Malformed IPC packet: " << e.what();
                    }
                });

                this_thread::sleep_for(chrono::milliseconds{10});
            }
        }};
    }

    // Load images passed via command line in the background prior to
    // creating our main application such that they are not stalled
    // by the potentially slow initialization of opengl / glfw.
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        imagesLoader->enqueue(imageFile, channelSelector, false);
    }

    // Init nanogui application
    nanogui::init();

#ifdef __APPLE__
    if (!imageFiles) {
        // If we didn't get any command line arguments for files to open,
        // then, on macOS, they might have been supplied through the NS api.
        const char* const* openedFiles = glfwGetOpenedFilenames();
        if (openedFiles) {
            for (auto p = openedFiles; *p; ++p) {
                imagesLoader->enqueue(*p, "", false);
            }
        }
    }
#endif

    auto [capability10bit, capabilityEdr] = nanogui::test_10bit_edr_support();
    if (get(ldrFlag)) {
        capability10bit = false;
        capabilityEdr = false;
    }

    tlog::info() << "Launching with " << (capability10bit ? 10 : 8) << " bits of color and " << (capabilityEdr ? "HDR" : "LDR") << " display support.";

    // Do what the maximize flag tells us---if it exists---and
    // maximize if we have images otherwise.
    bool maximize = maximizeFlag ? get(maximizeFlag) : imageFiles;

    // sImageViewer is a raw pointer to make sure it will never
    // get deleted. nanogui crashes upon cleanup, so we better
    // not try.
    sImageViewer = new ImageViewer{imagesLoader, maximize, capability10bit || capabilityEdr, capabilityEdr};

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

    // Refresh only every 250ms if there are no user interactions.
    // This makes an idling tev surprisingly energy-efficient. :)
    nanogui::mainloop(250);

    shallShutdown = true;

    // On some linux distributions glfwTerminate() (which is called by
    // nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
    //nanogui::shutdown();

    if (ipcThread.joinable()) {
        ipcThread.join();
    }

    if (stdinThread.joinable()) {
        stdinThread.join();
    }

    return 0;
}

TEV_NAMESPACE_END

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
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

        tev::mainFunc(arguments);
    } catch (const exception& e) {
        tlog::error() << tfm::format("Uncaught exception: %s", e.what());
        return 1;
    }
}
