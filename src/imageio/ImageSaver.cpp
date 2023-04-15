// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ImageSaver.h>

#include <tev/imageio/ExrImageSaver.h>
#include <tev/imageio/QoiImageSaver.h>
#include <tev/imageio/StbiHdrImageSaver.h>
#include <tev/imageio/StbiLdrImageSaver.h>

#include <vector>

using namespace std;

namespace tev {

const vector<unique_ptr<ImageSaver>>& ImageSaver::getSavers() {
    auto makeSavers = [] {
        vector<unique_ptr<ImageSaver>> imageSavers;
        imageSavers.emplace_back(new ExrImageSaver());
        imageSavers.emplace_back(new QoiImageSaver());
        imageSavers.emplace_back(new StbiHdrImageSaver());
        imageSavers.emplace_back(new StbiLdrImageSaver());
        return imageSavers;
    };

    static const vector imageSavers = makeSavers();
    return imageSavers;
}

}
