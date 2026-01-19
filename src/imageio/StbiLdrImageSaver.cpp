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

#include <tev/imageio/StbiLdrImageSaver.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <ostream>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

void StbiLdrImageSaver::save(ostream& oStream, const fs::path& path, span<const uint8_t> data, const Vector2i& imageSize, int nChannels) const {
    static const auto stbiOStreamWrite = [](void* context, void* stbidata, int size) {
        reinterpret_cast<ostream*>(context)->write(reinterpret_cast<char*>(stbidata), size);
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
        throw ImageSaveError{fmt::format("Image {} has unknown format.", path)};
    }
}

} // namespace tev
