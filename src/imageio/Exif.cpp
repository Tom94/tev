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
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/Exif.h>

#include <libexif/exif-data.h>

#include <vector>

using namespace std;

namespace tev {

namespace {
ExifByteOrder byteOrder(bool reverseEndianness) { return reverseEndianness ? EXIF_BYTE_ORDER_MOTOROLA : EXIF_BYTE_ORDER_INTEL; }
} // namespace

Exif::Exif() {
    ScopeGuard guard{[this]() { reset(); }};

    mExif = exif_data_new();
    if (!mExif) {
        throw invalid_argument{"Failed to init EXIF decoder."};
    }

    mExifLog = exif_log_new();
    if (!mExifLog) {
        throw invalid_argument{"Failed to init EXIF log."};
    }

    mExifLogError = make_unique<bool>(false);

    exif_log_set_func(
        mExifLog,
        [](ExifLog*, ExifLogCode kind, const char* domain, const char* format, va_list args, void* userData) {
            bool* error = static_cast<bool*>(userData);

            char buf[1024];
            vsnprintf(buf, sizeof(buf), format, args);
            string msg = fmt::format("{}: {}", domain, buf);
            switch (kind) {
                case EXIF_LOG_CODE_NONE: tlog::info() << msg; break;
                case EXIF_LOG_CODE_DEBUG: tlog::debug() << msg; break;
                case EXIF_LOG_CODE_NO_MEMORY:
                    *error = true;
                    tlog::error() << msg;
                    break;
                case EXIF_LOG_CODE_CORRUPT_DATA:
                    *error = true;
                    tlog::error() << msg;
                    break;
            }
        },
        mExifLogError.get()
    );

    exif_data_log(mExif, mExifLog);

    guard.disarm();
}

Exif::Exif(span<const uint8_t> exifData, bool autoPrependFourcc) : Exif() {
    ScopeGuard guard{[this]() { reset(); }};

    // If data doesn't already start with fourcc, prepend
    vector<uint8_t> newExifData;
    if (autoPrependFourcc && (exifData.size() < 6 || memcmp(exifData.data(), Exif::FOURCC.data(), 6) != 0)) {
        newExifData.reserve(exifData.size() + FOURCC.size());
        newExifData.insert(newExifData.end(), FOURCC.begin(), FOURCC.end());
        newExifData.insert(newExifData.end(), exifData.begin(), exifData.end());
        exifData = newExifData;
    }

    if (exifData.size() > numeric_limits<unsigned int>::max()) {
        throw invalid_argument{"EXIF data size exceeds maximum supported size."};
    }

    exif_data_load_data(mExif, exifData.data(), (unsigned int)exifData.size());

    if (*mExifLogError) {
        throw invalid_argument{"Failed to decode EXIF data."};
    }

    // Uncomment to dump complete EXIF contents
    // tlog::debug() << "Loaded EXIF data. Entries:";
    // if (tlog::Logger::global()->hiddenSeverities().count(tlog::ESeverity::Debug) == 0) {
    //     exif_data_dump(mExif);
    // }

    auto exifByteOrder = exif_data_get_byte_order(mExif);
    auto systemByteOrder = endian::native == std::endian::little ? EXIF_BYTE_ORDER_INTEL : EXIF_BYTE_ORDER_MOTOROLA;
    mReverseEndianess = exifByteOrder != systemByteOrder;

    guard.disarm();
}

Exif::~Exif() { reset(); }

void Exif::reset() {
    if (mExifLog) {
        exif_log_unref(mExifLog);
        mExifLog = nullptr;
    }

    if (mExif) {
        exif_data_unref(mExif);
        mExif = nullptr;
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

AttributeNode Exif::toAttributes() const {
    AttributeNode result;
    result.name = "EXIF";

    for (int ifd = EXIF_IFD_0; ifd < EXIF_IFD_COUNT; ++ifd) {
        ExifContent* content = mExif->ifd[ifd];
        if (content->count == 0) {
            continue;
        }

        AttributeNode& ifdNode = result.children.emplace_back();

        ifdNode.name = exif_ifd_get_name((ExifIfd)ifd);
        ifdNode.type = "IFD";

        for (size_t i = 0; i < content->count; ++i) {
            ExifEntry* entry = content->entries[i];

            const char* name = exif_tag_get_name_in_ifd(entry->tag, (ExifIfd)ifd);
            if (!name) {
                continue;
            }

            const char* type = exif_format_get_name(entry->format);
            if (!type) {
                type = "unknown";
            }

            char buf[256] = {0};
            string value = exif_entry_get_value(entry, buf, sizeof(buf));
            if (value.empty()) {
                value = "n/a";
            } else if (value.length() >= 255) {
                value += "…"s;
            }

            ifdNode.children.push_back({name, value, type, {}});
        }
    }

    return result;
}

} // namespace tev
