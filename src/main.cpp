// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ImageViewer.h>
#include <tev/Ipc.h>
#include <tev/SharedQueue.h>
#include <tev/ThreadPool.h>

#include <args.hxx>
#include <ImfThreading.h>

#include <utf8.h>

#include <iostream>
#include <thread>

using namespace args;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

int mainFunc(const vector<string>& arguments) {
    ArgumentParser parser{
        "Inspection tool for images with a high dynamic range.",
        "tev was developed by Thomas Müller <thomas94@gmx.net>. "
        "Its source code is available under the BSD 3-Clause License at https://tom94.net",
    };

    HelpFlag helpFlag{
        parser,
        "HELP",
        "Display this help menu",
        {'h', "help"},
    };

    Flag versionFlag{
        parser,
        "VERSION",
        "Display the version of tev",
        {'v', "version"},
    };

    Flag newWindowFlag{
        parser,
        "NEW WINDOW",
        "Open a new window of tev, even if one exists already",
        {'n', "new"},
    };

    ValueFlag<float> exposureFlag{
        parser,
        "EXPOSURE",
        "Scales the brightness of an image prior to tonemapping by 2^EXPOSURE. "
        "It can be controlled via the GUI, or by pressing E/Shift+E.",
        {'e', "exposure"},
    };

    ValueFlag<string> filterFlag{
        parser,
        "FILTER",
        "Filter visible images and layers according to a supplied string. "
        "The string must have the format 'image:layer'. "
        "Only images whose name contains 'image' and layers whose name contains 'layer' will be visible.",
        {'f', "filter"},
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

    ValueFlag<float> offsetFlag{
        parser,
        "OFFSET",
        "Add an absolute offset to the image after EXPOSURE has been applied. "
        "It can be controlled via the GUI, or by pressing O/Shift+O.",
        {'o', "offset"},
    };

    ValueFlag<string> tonemapFlag{
        parser,
        "TONEMAP",
        "The tonemapping algorithm to use. "
        "The available tonemaps are:\n"
        "sRGB   - sRGB\n"
        "Gamma  - Gamma curve (2.2)\n"
        "FC     - False Color\n"
        "PN     - Positive=Green, Negative=Red\n"
        "Default is sRGB.",
        {'t', "tonemap"},
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
    } catch (Help) {
        std::cout << parser;
        return 0;
    } catch (ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -1;
    } catch (ValidationError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -2;
    }

    if (versionFlag) {
        cout
            << "tev — The EXR Viewer" << endl
            << "version " TEV_VERSION << endl;
        return 0;
    }

    auto ipc = make_shared<Ipc>();

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
                ipc->sendToPrimaryInstance(tfm::format("%s:%s", path{imageFile}.make_absolute(), channelSelector));
            } catch (runtime_error e) {
                cerr << tfm::format("Invalid file '%s': %s", imageFile, e.what()) << endl;
            }
        }

        return 0;
    }

    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    cout << "Loading window..." << endl;

    // Load images passed via command line in the background prior to
    // creating our main application such that they are not stalled
    // by the potentially slow initialization of opengl / glfw.
    shared_ptr<SharedQueue<ImageAddition>> imagesToAdd = make_shared<SharedQueue<ImageAddition>>();
    string channelSelector;
    for (auto imageFile : get(imageFiles)) {
        if (!imageFile.empty() && imageFile[0] == ':') {
            channelSelector = imageFile.substr(1);
            continue;
        }

        ThreadPool::singleWorker().enqueueTask([imageFile, channelSelector, &imagesToAdd] {
            auto image = tryLoadImage(imageFile, channelSelector);
            if (image) {
                imagesToAdd->push({false, image});
            }
        });
    }

    // Init nanogui application
    nanogui::init();

    {
        auto app = unique_ptr<ImageViewer>{new ImageViewer{ipc, imagesToAdd, !imageFiles}};
        app->drawAll();
        app->setVisible(true);

        // Do what the maximize flag tells us---if it exists---and
        // maximize if we have images otherwise.
        if (maximizeFlag ? get(maximizeFlag) : imageFiles) {
            app->maximize();
        }

        // Apply parameter flags
        if (exposureFlag) { app->setExposure(get(exposureFlag)); }
        if (filterFlag)   { app->setFilter(get(filterFlag)); }
        if (metricFlag)   { app->setMetric(toMetric(get(metricFlag))); }
        if (offsetFlag)   { app->setOffset(get(offsetFlag)); }
        if (tonemapFlag)  { app->setTonemap(toTonemap(get(tonemapFlag))); }

        // Refresh only every 250ms if there are no user interactions.
        // This makes an idling tev surprisingly energy-efficient. :)
        nanogui::mainloop(250);
    }

    // On some linux distributions glfwTerminate() (which is called by
    // nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
    //nanogui::shutdown();

    // Let all threads gracefully terminate.
    ThreadPool::shutdown();

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
        cerr << tfm::format("Uncaught exception: %s", e.what()) << endl;
        return 1;
    }
}
