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

#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>

#include <istream>
#include <span>

namespace tev {

class JxlImageLoader : public ImageLoader {
public:
    Task<std::vector<ImageData>> load(
        std::istream& iStream, const fs::path& path, std::string_view channelSelector, const ImageLoaderSettings& settings, int priority
    ) const override;

    Task<std::vector<ImageData>> load(
        std::span<const uint8_t> data,
        const fs::path& path,
        std::string_view channelSelector,
        const ImageLoaderSettings& settings,
        int priority,
        bool skipColorProcessing,
        size_t* bitsPerSampleOut = nullptr,
        EPixelType* pixelTypeOut = nullptr
    ) const;

    std::string name() const override { return "JPEG XL"; }
};

} // namespace tev
