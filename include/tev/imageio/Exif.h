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

struct _ExifData;
struct _ExifLog;

namespace tev {

enum EExifLightSource : uint16_t {
    Unknown = 0,
    Daylight = 1,
    Fluorescent = 2,
    TungstenIncandescent = 3,
    Flash = 4,
    FineWeather = 9,
    Cloudy = 10,
    Shade = 11,
    DaylightFluorescent = 12,
    DayWhiteFluorescent = 13,
    CoolWhiteFluorescent = 14,
    WhiteFluorescent = 15,
    WarmWhiteFluorescent = 16,
    StandardLightA = 17,
    StandardLightB = 18,
    StandardLightC = 19,
    D55 = 20,
    D65 = 21,
    D75 = 22,
    D50 = 23,
    ISOStudioTungsten = 24,
    Other = 255,
};

std::string toString(EExifLightSource lightSource);

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
