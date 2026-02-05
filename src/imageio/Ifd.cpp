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

#include <tev/Common.h>
#include <tev/imageio/Ifd.h>

#include <optional>

using namespace std;

namespace tev {

Ifd::Ifd(std::span<const uint8_t> data, size_t initialOffset, bool tiffHeader, optional<bool> reverseEndianess) {
    const uint8_t* ptr = data.data();

    size_t ofs = initialOffset;

    if (reverseEndianess.has_value()) {
        mReverseEndianess = *reverseEndianess;
    } else {
        if ((data[ofs] == 'M') && (data[ofs + 1] == 'M')) {
            mReverseEndianess = std::endian::little == std::endian::native;
        } else if ((data[ofs] == 'I') && (data[ofs + 1] == 'I')) {
            mReverseEndianess = std::endian::big == std::endian::native;
        } else {
            throw invalid_argument{"IFD: failed to determine byte order."};
        }

        ofs += 2;
    }

    if (tiffHeader) {
        const uint16_t magic = read<uint16_t>(ptr + ofs);
        ofs += 2;

        if (magic != 42) {
            throw invalid_argument{"IFD: invalid TIFF magic."};
        }

        const uint32_t ifdOffset = read<uint32_t>(ptr + ofs);

        tlog::debug() << fmt::format("IFD: ifdOffset={}", ifdOffset);
        ofs = initialOffset + ifdOffset;
    }

    const uint32_t tcount = read<uint16_t>(ptr + ofs);
    ofs += 2;

    if (ofs + tcount * 12 + 4 > data.size()) {
        throw invalid_argument{fmt::format("IFD: too short for {} tags.", tcount)};
    }

    tlog::debug() << "Decoding IFD:";
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

        const auto d = data.subspan(entryOffset, entry.size());
        entry.data.insert(entry.data.end(), d.begin(), d.end());

        if (entryOffset + entry.size() > data.size()) {
            throw invalid_argument{fmt::format("IFD: offset overflow {}+{} vs. {}", entryOffset, entry.size(), data.size())};
        }

        ofs += 12;
        mTags[entry.tag] = entry;

        tlog::debug() << fmt::format("  tag={} format={} components={}", entry.tag, (int)entry.format, entry.nComponents);
    }

    if (ofs + 4 <= data.size()) {
        const auto nextIfdOffset = read<uint32_t>(ptr + ofs);
        if (nextIfdOffset != 0) {
            tlog::debug() << fmt::format("IFD: next IFD offset: {}", nextIfdOffset);
            mNextIfdOffset = nextIfdOffset;
        }
    }
}

} // namespace tev
