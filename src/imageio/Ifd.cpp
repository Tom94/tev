/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
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
#include <tev/imageio/Ifd.h>

#include <optional>

using namespace std;

namespace tev {

Ifd::Ifd(span<const uint8_t> data, size_t initialOffset, bool tiffHeader, optional<bool> reverseEndianess) {
    const uint8_t* ptr = data.data();

    size_t ofs = initialOffset;

    if (reverseEndianess.has_value()) {
        mReverseEndianess = *reverseEndianess;
    } else {
        if (ofs + 2 > data.size()) {
            throw invalid_argument{"IFD: too short for byte order."};
        }

        if ((data[ofs] == 'M') && (data[ofs + 1] == 'M')) {
            mReverseEndianess = endian::little == endian::native;
        } else if ((data[ofs] == 'I') && (data[ofs + 1] == 'I')) {
            mReverseEndianess = endian::big == endian::native;
        } else {
            throw invalid_argument{"IFD: failed to determine byte order."};
        }

        ofs += 2;
    }

    if (tiffHeader) {
        if (ofs + 6 > data.size()) {
            throw invalid_argument{"IFD: too short for TIFF header."};
        }

        mTiffMagic[0] = data[ofs + 0];
        mTiffMagic[1] = data[ofs + 1];
        ofs += 2;
        tlog::debug("IFD: magic={}", mTiffMagic);

        const uint32_t ifdOffset = read<uint32_t>(ptr + ofs);
        tlog::debug("IFD: ifdOffset={}", ifdOffset);
        ofs = initialOffset + ifdOffset;
    }

    if (ofs + 2 > data.size()) {
        throw invalid_argument{"IFD: too short for tag count."};
    }

    const uint32_t tcount = read<uint16_t>(ptr + ofs);
    ofs += 2;

    if (ofs + (size_t)tcount * 12 + 4 > data.size()) {
        throw invalid_argument{fmt::format("IFD: too short for {} tags.", tcount)};
    }

    tlog::debug("Decoding IFD with {} elements:", tcount);
    for (uint32_t i = 0; i < tcount; i++) {
        if (ofs + 12 > data.size()) {
            throw invalid_argument{"IFD: overflow"};
        }

        TiffTag entry;
        entry.tag = read<uint16_t>(ptr + ofs);
        entry.format = read<TiffTag::EFormat>(ptr + ofs + 2);
        entry.nComponents = read<uint32_t>(ptr + ofs + 4);

        if (ofs + 4 + entry.size() > data.size()) {
            throw invalid_argument{"IFD: elem overflow"};
        }

        size_t entryOffset;
        if (entry.size() > 4) {
            // Entry is stored somewhere else, pointed to by the following
            entryOffset = read<uint32_t>(ptr + ofs + 8); // -6?
        } else {
            entryOffset = ofs + 8;
        }

        if (entryOffset + entry.size() > data.size()) {
            throw invalid_argument{fmt::format("IFD: offset overflow {}+{} vs. {}", entryOffset, entry.size(), data.size())};
        }

        const auto d = data.subspan(entryOffset, entry.size());
        entry.data.insert(entry.data.end(), d.begin(), d.end());

        ofs += 12;
        mTags[entry.tag] = entry;

        tlog::debug("  tag={:04X}/{} format={} components={}", entry.tag, entry.tag, (int)entry.format, entry.nComponents);
    }

    if (ofs + 4 <= data.size()) {
        const auto nextIfdOffset = read<uint32_t>(ptr + ofs);
        if (nextIfdOffset != 0) {
            tlog::debug("IFD: next IFD offset: {}", nextIfdOffset);
            mNextIfdOffset = nextIfdOffset;
        }
    }
}

} // namespace tev
