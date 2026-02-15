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
#include <span>

namespace tev {

class JxlImageLoader : public ImageLoader {
public:
    Task<std::vector<ImageData>> load(
        std::istream& iStream, const fs::path& path, std::string_view channelSelector, int priority, const GainmapHeadroom& gainmapHeadroom
    ) const override;

    Task<std::vector<ImageData>> load(
        std::span<const uint8_t> data,
        const fs::path& path,
        std::string_view channelSelector,
        int priority,
        const GainmapHeadroom& gainmapHeadroom,
        bool skipColorProcessing,
        // Ability to override the true number of bits per sample in the image. E.g. when a 16 bit JXL encodes data with only 12 bits of
        // precision, as can happen when JXL codestreams are embedded in TIFF files. This argument affects scaling of the decoded float
        // data: the maximum possible value will be 2^bitsPerSampleOverride-1.
        // NOTE: this parameter only has an effect if no ICC profile is present
        std::optional<uint32_t> bitsPerSampleOverride = std::nullopt
    ) const;

    std::string name() const override { return "JPEG XL"; }
};

} // namespace tev
