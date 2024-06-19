// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/StbiLdrImageSaver.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <ostream>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

void StbiLdrImageSaver::save(ostream& oStream, const fs::path& path, const vector<char>& data, const Vector2i& imageSize, int nChannels) const {
    static const auto stbiOStreamWrite = [](void* context, void* data, int size) {
        reinterpret_cast<ostream*>(context)->write(reinterpret_cast<char*>(data), size);
    };

    auto extension = toLower(toString(path.extension()));

    if (extension == ".jpg" || extension == ".jpeg") {
        stbi_write_jpg_to_func(stbiOStreamWrite, &oStream, imageSize.x(), imageSize.y(), nChannels, data.data(), 100);
    } else if (extension == ".png") {
        stbi_write_png_to_func(stbiOStreamWrite, &oStream, imageSize.x(), imageSize.y(), nChannels, data.data(), 0);
    } else if (extension == ".bmp") {
        stbi_write_bmp_to_func(stbiOStreamWrite, &oStream, imageSize.x(), imageSize.y(), nChannels, data.data());
    } else if (extension == ".tga") {
        stbi_write_tga_to_func(stbiOStreamWrite, &oStream, imageSize.x(), imageSize.y(), nChannels, data.data());
    } else {
        throw invalid_argument{fmt::format("Image {} has unknown format.", path)};
    }
}

}
