// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/ImageViewer.h"

#include <args.hxx>
#include <ImfThreading.h>

#include <iostream>
#include <thread>

using namespace args;
using namespace std;

TEV_NAMESPACE_BEGIN

int mainFunc(int argc, char* argv[]) {
    Imf::setGlobalThreadCount(thread::hardware_concurrency());

    ArgumentParser parser{
        "Inspection tool for images with a high dynamic range.",
        "",
    };

    HelpFlag helpFlag{
        parser,
        "help",
        "Display this help menu",
        {'h', "help"},
    };

    ValueFlag<float> exposureFlag{
        parser,
        "exposure",
        "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure. "
        "It can be controlled via the GUI, or by pressing E/Shift+E.",
        {'e', "exposure"},
    };

    ValueFlag<string> filterFlag{
        parser,
        "filter",
        "Filters visible images and layers according to a supplied string. "
        "The string should have the format 'image#layer'. "
        "Only images whose name contains 'image' and layers whose name contains 'layer' will be visible.",
        {'f', "filter"},
    };

    ValueFlag<bool> maximizeFlag{
        parser,
        "maximize",
        "Whether to maximize the window on startup or not. "
        "If no images were supplied via the command line, then the default is false. "
        "Otherwise, the default is true.",
        {'m', "maximize"},
    };

    ValueFlag<float> offsetFlag{
        parser,
        "offset",
        "The offset is added to the image after exposure has been applied. "
        "It can be controlled via the GUI, or by pressing O/Shift+O.",
        {'o', "offset"},
    };

    PositionalList<string> imageFiles{
        parser,
        "images",
        "The image files to be opened by the viewer.",
    };

    // Parse command line arguments and react to parsing
    // errors using exceptions.
    try {
        parser.ParseCLI(argc, argv);
    } catch (args::Help) {
        std::cout << parser;
        return 0;
    } catch (args::ParseError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -1;
    } catch (args::ValidationError e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return -2;
    }

    // Load images passed via command line prior to initializing nanogui
    // such that no frozen window is created.
    vector<shared_ptr<Image>> images;
    for (const auto imageFile : get(imageFiles)) {
        auto image = tryLoadImage(imageFile);
        if (image) {
            images.emplace_back(image);
        }
    }

    // Init nanogui application
    nanogui::init();

    {
        auto app = make_unique<ImageViewer>();
        app->drawAll();
        app->setVisible(true);

        bool shallMaximize = false;

        // Load all images which were passed in via the command line.
        if (!images.empty()) {
            for (const auto& image : images) {
                app->addImage(image);
            }

            // If all images were loaded from the command line, then there
            // is a good chance the user won't want to interact with the OS
            // to drag more images in. Therefore, let's maximize by default.
            shallMaximize = true;
        }

        // Override shallMaximize according to the supplied flag
        if (maximizeFlag) {
            shallMaximize = get(maximizeFlag);
        }

        // Make sure the largest loaded image fits into our window.
        app->fitAllImages();
        if (shallMaximize) {
            app->maximize();
        }

        // Adjust exposure according to potential command line parameters.
        if (exposureFlag) {
            app->setExposure(get(exposureFlag));
        }

        if (offsetFlag) {
            app->setOffset(get(offsetFlag));
        }

        if (filterFlag) {
            app->setFilter(get(filterFlag));
        }

        nanogui::mainloop();
    }

    // On some linux distributions glfwTerminate() (which is called by
    // nanogui::shutdown()) causes segfaults. Since we are done with our
    // program here anyways, let's let the OS clean up after us.
    //nanogui::shutdown();

    return 0;
}

TEV_NAMESPACE_END

int main(int argc, char* argv[]) {
    try {
        tev::mainFunc(argc, argv);
    } catch (const runtime_error& e) {
        cerr << "Uncaught exception: "s + e.what() << endl;
        return 1;
    }
}
