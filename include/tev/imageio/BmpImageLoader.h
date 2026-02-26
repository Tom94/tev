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

#pragma once

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>

#include <istream>
#include <optional>

namespace tev {

class BmpImageLoader : public ImageLoader {
public:
    Task<std::vector<ImageData>> loadWithoutFileHeader(
        std::istream& iStream,
        const fs::path& path,
        std::string_view channelSelector,
        const ImageLoaderSettings& settings,
        int priority,
        std::optional<size_t> pixelDataOffset,
        // If provided, this will be used as the image size instead of the size specified in the BMP headers. The value will be modified to
        // the size from the bmp header in case of a mismatch, so the caller can detect that and react accordingly. Necessary for loading
        // ICO files where embedded BMP images have twice the size specified in the header to cover the AND mask
        nanogui::Vector2i* sizeInOut = nullptr,
        // BMP files don't read the alpha channel (even if data permits) by default. However, in some cases (e.g. ICO files) the alpha
        // channel is actually used even if there is no DIB header saying as much and should be read by default. This parameter controls
        // that behavior.
        bool alphaByDefault = false
    ) const;

    Task<std::vector<ImageData>> load(
        std::istream& iStream, const fs::path& path, std::string_view channelSelector, const ImageLoaderSettings& settings, int priority
    ) const override;

    std::string name() const override { return "BMP"; }
};

} // namespace tev
