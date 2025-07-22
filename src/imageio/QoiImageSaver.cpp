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

#include <tev/imageio/QoiImageSaver.h>

#define QOI_NO_STDIO
#include <qoi.h>

#include <ostream>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

void QoiImageSaver::save(ostream& oStream, const fs::path&, span<const char> data, const Vector2i& imageSize, int nChannels) const {
    // The QOI image format expects nChannels to be either 3 for RGB data or 4 for RGBA.
    if (nChannels != 4 && nChannels != 3) {
        throw ImageSaveError{fmt::format("Invalid number of channels {}.", nChannels)};
    }

    const qoi_desc desc{
        .width = static_cast<unsigned int>(imageSize.x()),
        .height = static_cast<unsigned int>(imageSize.y()),
        .channels = static_cast<unsigned char>(nChannels),
        .colorspace = QOI_SRGB,
    };
    int sizeInBytes = 0;
    void* encodedData = qoi_encode(data.data(), &desc, &sizeInBytes);

    ScopeGuard encodedDataGuard{[encodedData] { free(encodedData); }};

    if (!encodedData) {
        throw ImageSaveError{"Failed to encode data into the QOI format."};
    }

    oStream.write(reinterpret_cast<char*>(encodedData), sizeInBytes);
}

} // namespace tev
