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

#include <cstdint>
#include <span>

namespace tev {

class IsoGainMapVersion {
public:
    IsoGainMapVersion() = default;
    IsoGainMapVersion(std::span<const uint8_t> data, size_t* pos = nullptr);
    IsoGainMapVersion(std::string_view v) : mVersionString{v} {}

    const std::string& toString() const { return mVersionString; }
    void setString(const std::string& v) { mVersionString = v; }

private:
    std::string mVersionString = "n/a";
};

class IsoGainMapMetadata {
public:
    IsoGainMapMetadata(std::span<const uint8_t> data); // Construct from bytes per ISO 21496-1
    IsoGainMapMetadata(const char* ns, void* xmpMeta); // Construct from Adobe XMP metadata

    template <typename T> static T read(std::span<const uint8_t> data, size_t* pos = nullptr) {
        static constexpr bool reverseEndianess = std::endian::native == std::endian::little;

        const size_t offset = pos ? *pos : 0;
        if (data.size() < offset + sizeof(T)) {
            throw std::invalid_argument{"Not enough data to read value."};
        }

        const uint8_t* ptr = data.data();
        if (pos) {
            ptr += *pos;
            *pos += sizeof(T);
        }

        T result = *reinterpret_cast<const T*>(ptr);
        if (reverseEndianess) {
            result = swapBytes(result);
        }

        return result;
    }

    AttributeNode toAttributes() const;

    const IsoGainMapVersion& version() const { return mVersion; }

    const nanogui::Vector3f& gainMapMin() const { return mGainMapMin; }
    const nanogui::Vector3f& gainMapMax() const { return mGainMapMax; }
    const nanogui::Vector3f& gainMapGamma() const { return mGainMapGamma; }

    const nanogui::Vector3f& baseOffset() const { return mBaseOffset; }
    const nanogui::Vector3f& alternateOffset() const { return mAlternateOffset; }

    float baseHdrHeadroom() const { return mBaseHdrHeadroom; }
    float alternateHdrHeadroom() const { return mAlternateHdrHeadroom; }

    bool useBaseColorSpace() const { return mUseBaseColorSpace; }

private:
    void reverseDirection();

    IsoGainMapVersion mVersion;

    nanogui::Vector3f mGainMapMin;
    nanogui::Vector3f mGainMapMax;
    nanogui::Vector3f mGainMapGamma;

    nanogui::Vector3f mBaseOffset;
    nanogui::Vector3f mAlternateOffset;

    float mBaseHdrHeadroom;
    float mAlternateHdrHeadroom;

    bool mUseBaseColorSpace = false;
};

} // namespace tev
