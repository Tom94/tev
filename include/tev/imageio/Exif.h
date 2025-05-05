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
#include <tev/imageio/AppleMakerNote.h>

#include <cstdint>
#include <vector>

struct _ExifData;

namespace tev {

class Exif {
public:
    Exif(std::vector<uint8_t> exifData);
    ~Exif();

    AppleMakerNote tryGetAppleMakerNote() const;

    EOrientation getOrientation() const;

private:
    bool mReverseEndianess = false;
    _ExifData* mExif = nullptr;
};

} // namespace tev
