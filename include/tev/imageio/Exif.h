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
#include <tev/imageio/AppleMakerNote.h>

#include <array>
#include <cstdint>
#include <span>

struct _ExifData;
struct _ExifLog;

namespace tev {

class Exif {
public:
    static constexpr std::array<uint8_t, 6> FOURCC = {
        'E',
        'x',
        'i',
        'f',
        '\0',
        '\0',
    };

    Exif();
    Exif(std::span<const uint8_t> exifData, bool autoPrependFourcc = true);
    ~Exif();

    Exif(const Exif&) = delete;
    Exif& operator=(const Exif&) = delete;
    Exif(Exif&& other) noexcept { *this = std::move(other); }
    Exif& operator=(Exif&& other) noexcept {
        std::swap(mReverseEndianess, other.mReverseEndianess);
        std::swap(mExif, other.mExif);
        std::swap(mExifLog, other.mExifLog);
        std::swap(mExifLogError, other.mExifLogError);
        return *this;
    }

    void reset();

    AppleMakerNote tryGetAppleMakerNote() const;

    EOrientation getOrientation() const;

    AttributeNode toAttributes() const;

private:
    bool mReverseEndianess = false;

    _ExifData* mExif = nullptr;
    _ExifLog* mExifLog = nullptr;
    std::unique_ptr<bool> mExifLogError = nullptr;
};

} // namespace tev
