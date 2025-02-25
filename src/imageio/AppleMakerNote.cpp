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

#include <tev/Common.h>
#include <tev/imageio/AppleMakerNote.h>

using namespace std;

namespace tev {

static const uint8_t APPLE_SIGNATURE[] = {0x41, 0x70, 0x70, 0x6C, 0x65, 0x20, 0x69, 0x4F, 0x53, 0x00}; // "Apple iOS\0"
static const size_t SIG_LENGTH = sizeof(APPLE_SIGNATURE);

bool isAppleMakernote(const uint8_t* data, size_t length) {

    if (length < SIG_LENGTH) {
        return false;
    }

    return memcmp(data, APPLE_SIGNATURE, SIG_LENGTH) == 0;
}

// This whole function is one huge hack. It was pieced together by referencing the EXIF spec as well as the (non-functional) implementation
// over at libexif. https://github.com/libexif/libexif/blob/master/libexif/apple/exif-mnote-data-apple.c That, plus quite a bit of trial and
// error, finally got this to work. Who knows when Apple will break it. :)
AppleMakerNote::AppleMakerNote(const uint8_t* data, size_t length) {
    mReverseEndianess = false;

    size_t ofs = 0;
    if ((data[ofs + 12] == 'M') && (data[ofs + 13] == 'M')) {
        mReverseEndianess = std::endian::little == std::endian::native;
    } else if ((data[ofs + 12] == 'I') && (data[ofs + 13] == 'I')) {
        mReverseEndianess = std::endian::big == std::endian::native;
    } else {
        throw invalid_argument{"Failed to determine byte order."};
    }

    uint32_t tcount = read<uint16_t>(data + ofs + 14, mReverseEndianess);

    if (length < ofs + 16 + tcount * 12 + 4) {
        throw invalid_argument{"Too short"};
    }

    ofs += 16;

    tlog::debug() << "Decoding Apple maker note:";
    for (uint32_t i = 0; i < tcount; i++) {
        if (ofs + 12 > length) {
            throw invalid_argument{"Overflow"};
        }

        AppleMakerNoteEntry entry;
        entry.tag = read<uint16_t>(data + ofs, mReverseEndianess);
        entry.format = read<AppleMakerNoteEntry::EFormat>(data + ofs + 2, mReverseEndianess);
        entry.nComponents = read<uint32_t>(data + ofs + 4, mReverseEndianess);

        if (ofs + 4 + entry.size() > length) {
            throw invalid_argument{"Elem overflow"};
        }

        size_t entryOffset;
        if (entry.size() > 4) {
            // Entry is stored somewhere else, pointed to by the following
            entryOffset = read<uint32_t>(data + ofs + 8, mReverseEndianess); // -6?
        } else {
            entryOffset = ofs + 8;
        }

        entry.data = vector<uint8_t>(data + entryOffset, data + entryOffset + entry.size());

        if (entryOffset + entry.size() > length) {
            throw invalid_argument{"Offset overflow"};
        }

        ofs += 12;
        mTags[entry.tag] = entry;

        tlog::debug() << fmt::format("  tag={} format={} components={}", entry.tag, (int)entry.format, entry.nComponents);
    }
}

} // namespace tev
