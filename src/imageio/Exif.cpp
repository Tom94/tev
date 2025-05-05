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

#include "libexif/exif-byte-order.h"
#include <tev/Common.h>
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/Exif.h>

#include <libexif/exif-data.h>

#include <vector>

using namespace std;

namespace tev {

namespace {
ExifByteOrder byteOrder(bool reverseEndianness) { return reverseEndianness ? EXIF_BYTE_ORDER_MOTOROLA : EXIF_BYTE_ORDER_INTEL; }
} // namespace

Exif::Exif(vector<uint8_t> exifData) {
    mExif = exif_data_new_from_data(exifData.data(), exifData.size());
    if (!mExif) {
        throw invalid_argument{"Failed to decode EXIF data."};
    }

    tlog::debug() << "Loaded EXIF data. Entries:";
    if (tlog::Logger::global()->hiddenSeverities().count(tlog::ESeverity::Debug) == 0) {
        exif_data_dump(mExif);
    }

    auto exifByteOrder = exif_data_get_byte_order(mExif);
    auto systemByteOrder = endian::native == std::endian::little ? EXIF_BYTE_ORDER_INTEL : EXIF_BYTE_ORDER_MOTOROLA;
    mReverseEndianess = exifByteOrder != systemByteOrder;
}

Exif::~Exif() {
    if (mExif) {
        exif_data_unref(mExif);
    }
}

AppleMakerNote Exif::tryGetAppleMakerNote() const {
    ExifEntry* makerNote = exif_data_get_entry(mExif, EXIF_TAG_MAKER_NOTE);
    return AppleMakerNote(makerNote->data, makerNote->size);
}

EOrientation Exif::getOrientation() const {
    ExifEntry* orientationEntry = exif_content_get_entry(mExif->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!orientationEntry) {
        return EOrientation::None;
    }

    return (EOrientation)exif_get_short(orientationEntry->data, byteOrder(mReverseEndianess));
}

} // namespace tev
