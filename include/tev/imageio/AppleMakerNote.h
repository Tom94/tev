/*
 * tev -- the EXR viewer
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
#include <map>
#include <stdexcept>
#include <vector>

namespace tev {

bool isAppleMakernote(const uint8_t* data, size_t length);

template <typename T> T read(const uint8_t* data, bool reverseEndianness) {
    if (reverseEndianness) {
        T result;
        for (size_t i = 0; i < sizeof(T); ++i) {
            reinterpret_cast<uint8_t*>(&result)[i] = data[sizeof(T) - i - 1];
        }

        return result;
    } else {
        return *reinterpret_cast<const T*>(data);
    }
}

struct AppleMakerNoteEntry {
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

        throw std::invalid_argument{std::string{"Unknown format: "} + std::to_string((uint32_t)format)};
    }

    size_t size() const { return nComponents * formatSize(format); }
};

class AppleMakerNote {
public:
    AppleMakerNote(const uint8_t* data, size_t length);

    template <typename T> T tryGetFloat(uint16_t tag, T defaultValue) const {
        if (mTags.count(tag) == 0) {
            return defaultValue;
        }

        return getFloat<T>(tag);
    }

    template <typename T> T getFloat(uint16_t tag) const {
        if (mTags.count(tag) == 0) {
            throw std::invalid_argument{"Requested tag does not exist."};
        }

        const auto& entry = mTags.at(tag);
        const uint8_t* data = entry.data.data();

        switch (entry.format) {
            case AppleMakerNoteEntry::EFormat::Byte: return static_cast<T>(*data);
            case AppleMakerNoteEntry::EFormat::Short: return static_cast<T>(read<uint16_t>(data, mReverseEndianess));
            case AppleMakerNoteEntry::EFormat::Long: return static_cast<T>(read<uint32_t>(data, mReverseEndianess));
            case AppleMakerNoteEntry::EFormat::Rational: {
                uint32_t numerator = read<uint32_t>(data, mReverseEndianess);
                uint32_t denominator = read<uint32_t>(data + sizeof(uint32_t), mReverseEndianess);
                return static_cast<T>(numerator) / static_cast<T>(denominator);
            }
            case AppleMakerNoteEntry::EFormat::Sbyte: return static_cast<T>(*reinterpret_cast<const int8_t*>(data));
            case AppleMakerNoteEntry::EFormat::Sshort: return static_cast<T>(read<int16_t>(data, mReverseEndianess));
            case AppleMakerNoteEntry::EFormat::Slong: return static_cast<T>(read<int32_t>(data, mReverseEndianess));
            case AppleMakerNoteEntry::EFormat::Srational: {
                int32_t numerator = read<int32_t>(data, mReverseEndianess);
                int32_t denominator = read<int32_t>(data + sizeof(int32_t), mReverseEndianess);
                return static_cast<T>(numerator) / static_cast<T>(denominator);
            }
            case AppleMakerNoteEntry::EFormat::Float: return static_cast<T>(*reinterpret_cast<const float*>(data));
            case AppleMakerNoteEntry::EFormat::Double: return static_cast<T>(*reinterpret_cast<const double*>(data));
            case AppleMakerNoteEntry::EFormat::Ascii:
            case AppleMakerNoteEntry::EFormat::Undefined: throw std::invalid_argument{"Cannot convert this format to float."};
        }

        throw std::invalid_argument{"Unknown format."};
    }

private:
    std::map<uint8_t, AppleMakerNoteEntry> mTags;
    bool mReverseEndianess = false;
};

} // namespace tev
