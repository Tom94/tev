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

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace tev {

struct TiffTag {
    enum class EFormat : uint16_t {
        Byte = 1,
        Ascii = 2,
        Short = 3,
        Long = 4,
        Rational = 5,
        Sbyte = 6,
        Undefined = 7,
        Sshort = 8,
        Slong = 9,
        Srational = 10,
        Float = 11,
        Double = 12,
    };

    uint16_t tag;
    EFormat format;
    uint32_t nComponents;
    std::vector<uint8_t> data;

    static size_t formatSize(EFormat format) {
        switch (format) {
            case EFormat::Byte:
            case EFormat::Ascii:
            case EFormat::Sbyte:
            case EFormat::Undefined: return 1;
            case EFormat::Short:
            case EFormat::Sshort: return 2;
            case EFormat::Long:
            case EFormat::Slong:
            case EFormat::Float: return 4;
            case EFormat::Rational:
            case EFormat::Srational:
            case EFormat::Double: return 8;
            default:
                // The default size of 4 for unknown types is chosen to make parsing easier. Larger types would be stored at a remote
                // location with the 4 bytes interpreted as an offset, which may be invalid depending on the indended behavior of the
                // unknown type. Better play it safe and just read 4 bytes, leaving it to the user to know whether they represent an offset
                // or a meaningful value by themselves.
                return 4;
        }

        throw std::invalid_argument{fmt::format("TiffTag: unknown format: {}", (uint32_t)format)};
    }

    size_t size() const { return nComponents * formatSize(format); }
};

class Ifd {
public:
    Ifd(std::span<const uint8_t> data, size_t initialOffset = 0, bool tiffHeader = false, std::optional<bool> reverseEndianess = std::nullopt);

    template <typename T> T read(const uint8_t* data) const {
        auto result = fromBytes<T>(data);
        if (mReverseEndianess) {
            result = swapBytes(result);
        }

        return result;
    }

    const TiffTag* tag(uint16_t tag) const {
        const auto it = mTags.find(tag);
        if (it == mTags.end()) {
            return nullptr;
        }

        return &it->second;
    }

    std::optional<TiffTag::EFormat> format(uint16_t tag) const {
        const auto it = mTags.find(tag);
        if (it == mTags.end()) {
            throw std::invalid_argument{"IFD: requested tag does not exist."};
        }

        const auto& entry = it->second;
        return entry.format;
    }

    const uint8_t* data(uint16_t tag) const {
        const auto it = mTags.find(tag);
        if (it == mTags.end()) {
            return nullptr;
        }

        const auto& entry = it->second;
        return entry.data.data();
    }

    template <typename T> std::optional<T> tryGet(uint16_t tag) const {
        const auto it = mTags.find(tag);
        if (it == mTags.end()) {
            return std::nullopt;
        }

        const auto& entry = it->second;
        const uint8_t* data = entry.data.data();

        switch (entry.format) {
            case TiffTag::EFormat::Byte: return static_cast<T>(*data);
            case TiffTag::EFormat::Short: return static_cast<T>(read<uint16_t>(data));
            case TiffTag::EFormat::Long: return static_cast<T>(read<uint32_t>(data));
            case TiffTag::EFormat::Rational: {
                const auto numerator = read<uint32_t>(data);
                const auto denominator = read<uint32_t>(data + sizeof(uint32_t));
                return static_cast<T>(numerator) / static_cast<T>(denominator);
            }
            case TiffTag::EFormat::Sbyte: return static_cast<T>(fromBytes<int8_t>(data));
            case TiffTag::EFormat::Sshort: return static_cast<T>(read<int16_t>(data));
            case TiffTag::EFormat::Slong: return static_cast<T>(read<int32_t>(data));
            case TiffTag::EFormat::Srational: {
                const auto numerator = read<int32_t>(data);
                const auto denominator = read<int32_t>(data + sizeof(int32_t));
                return static_cast<T>(numerator) / static_cast<T>(denominator);
            }
            case TiffTag::EFormat::Float: return static_cast<T>(fromBytes<float>(data));
            case TiffTag::EFormat::Double: return static_cast<T>(fromBytes<double>(data));
            case TiffTag::EFormat::Ascii:
            case TiffTag::EFormat::Undefined: return std::nullopt;
        }

        return std::nullopt;
    }

    bool reverseEndianess() const { return mReverseEndianess; }
    std::optional<uint32_t> nextIfdOffset() const { return mNextIfdOffset; }

private:
    std::unordered_map<uint16_t, TiffTag> mTags;

    bool mReverseEndianess = false;
    std::optional<uint32_t> mNextIfdOffset;
};

} // namespace tev
