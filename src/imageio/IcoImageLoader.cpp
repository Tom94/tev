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

#include <tev/ThreadPool.h>
#include <tev/imageio/BmpImageLoader.h>
#include <tev/imageio/IcoImageLoader.h>
#include <tev/imageio/PngImageLoader.h>

using namespace nanogui;
using namespace std;

namespace tev {

template <typename T> static T read(const uint8_t* data, bool reverseEndianness) {
    T result = *reinterpret_cast<const T*>(data);
    if (reverseEndianness) {
        result = swapBytes(result);
    }

    return result;
}

Task<vector<ImageData>> IcoImageLoader::load(
    istream& iStream, const fs::path& path, string_view channelSelector, const ImageLoaderSettings& settings, int priority
) const {
    const size_t initialPos = iStream.tellg();
    const bool reverseEndianness = endian::native == endian::big;

    struct IconDir {
        struct Entry {
            int width;
            int height;
            uint8_t colorCount;
            uint8_t reserved;
            uint16_t planes;
            uint16_t bpp;
            uint32_t bytesInRes;
            uint32_t imageOffset;
        };

        uint16_t reserved;
        uint16_t type;
        uint16_t count;

        vector<Entry> entries;
    } dir;

    uint8_t header[6];
    iStream.read((char*)header, 6);

    if (!iStream) {
        throw FormatNotSupported{"Failed to read ICO/CUR header."};
    }

    dir.reserved = read<uint16_t>(header, reverseEndianness);
    dir.type = read<uint16_t>(header + 2, reverseEndianness);
    dir.count = read<uint16_t>(header + 4, reverseEndianness);

    enum class EType { Ico = 1, Cur = 2 };
    const auto typeToString = [](EType type) {
        switch (type) {
            case EType::Ico: return "ICO";
            case EType::Cur: return "CUR";
            default: return "unknown";
        }
    };

    const auto type = (EType)dir.type;

    if (dir.reserved != 0 || (type != EType::Cur && type != EType::Ico) || dir.count == 0) {
        throw FormatNotSupported{"Invalid ICO/CUR header"};
    }

    tlog::debug() << fmt::format("Loading {} images from {} container", dir.count, typeToString(type));

    dir.entries.resize(dir.count); // No need to sanitize. Worst case we allocate 64k entries, each 16 bytes
    for (uint16_t i = 0; i < dir.count; i++) {
        uint8_t entryData[16];
        iStream.read((char*)entryData, 16);
        if (!iStream) {
            throw FormatNotSupported{"Failed to read ICO/CUR entry."};
        }

        auto& entry = dir.entries[i];
        entry.width = entryData[0] == 0 ? 256 : entryData[0];
        entry.height = entryData[1] == 0 ? 256 : entryData[1];
        entry.colorCount = entryData[2];
        entry.reserved = entryData[3];
        entry.planes = read<uint16_t>(entryData + 4, reverseEndianness);
        entry.bpp = read<uint16_t>(entryData + 6, reverseEndianness);
        entry.bytesInRes = read<uint32_t>(entryData + 8, reverseEndianness);
        entry.imageOffset = read<uint32_t>(entryData + 12, reverseEndianness);
    }

    if (type == EType::Ico) {
        tlog::debug() << "Sorting ICO images by bitwidth, largest to smallest";
        sort(begin(dir.entries), end(dir.entries), [](const auto& a, const auto& b) {
            return tuple(a.bpp, a.width, a.height) > tuple(b.bpp, b.width, b.height);
        });
    }

    for (uint16_t i = 0; i < dir.count; i++) {
        const auto& entry = dir.entries[i];

        tlog::debug() << fmt::format(
            "  #{}: size={}x{} colorCount={} {}={} {}={} bytesInRes={} imageOffset={}",
            i,
            entry.width,
            entry.height,
            entry.colorCount,
            type == EType::Cur ? "hotspotX" : "planes",
            entry.planes,
            type == EType::Cur ? "hotspotY" : "bpp",
            entry.bpp,
            entry.bytesInRes,
            entry.imageOffset
        );
    }

    vector<ImageData> result;

    for (uint16_t i = 0; i < dir.count; i++) {
        const auto& entry = dir.entries[i];
        tlog::debug() << fmt::format("Loading image #{} from {} container", i, typeToString(type));

        vector<ImageData> imageData;
        try {
            iStream.clear();
            iStream.seekg(initialPos + entry.imageOffset, iStream.beg);
            if (!iStream) {
                throw FormatNotSupported{"Failed to seek to image data."};
            }

            const auto pngLoader = PngImageLoader{};
            imageData = co_await pngLoader.load(iStream, path, channelSelector, settings, priority);
        } catch (const FormatNotSupported&) {
            tlog::debug() << fmt::format("Not a PNG image; trying BMP.", i);
        } catch (const ImageLoadError& e) {
            tlog::warning() << fmt::format("Malformed PNG image: {}", i, e.what());
            continue;
        }

        if (imageData.empty()) {
            try {
                iStream.clear();
                iStream.seekg(initialPos + entry.imageOffset, iStream.beg);
                if (!iStream) {
                    throw FormatNotSupported{"Failed to seek to image data."};
                }

                // Potentially modified by loading the image. Will indicate if there is an AND mask following the image data that needs to
                // be applied to the image's alpha channel.
                auto size = Vector2i{entry.width, entry.height};

                const auto bmpLoader = BmpImageLoader{};
                imageData = co_await bmpLoader.loadWithoutFileHeader(iStream, path, channelSelector, settings, priority, nullopt, &size, true);

                if (size != Vector2i{entry.width, entry.height}) {
                    if (size != Vector2i{entry.width, entry.height * 2}) {
                        throw ImageLoadError{
                            fmt::format("BMP image size {} does not match entry size + AND mask {}x{}", size, entry.width, entry.height)
                        };
                    }

                    tlog::debug() << fmt::format("BMP image size {} indicates presence of AND mask. Applying...", size);

                    const auto bytesPerRow = (size_t)nextMultiple(entry.width, 32) / 8;
                    const size_t andMaskSize = bytesPerRow * entry.height;

                    const size_t maskDataPos = iStream.tellg();
                    iStream.seekg(0, ios::end);
                    const size_t maskDataEnd = iStream.tellg();
                    iStream.seekg(maskDataPos, ios_base::beg);

                    if (maskDataEnd - maskDataPos < andMaskSize) {
                        throw ImageLoadError{fmt::format(
                            "BMP file is too small to contain expected AND mask: {} bytes available, {} bytes expected",
                            maskDataEnd - maskDataPos,
                            andMaskSize
                        )};
                    }

                    HeapArray<uint8_t> andMaskData(andMaskSize);
                    iStream.read((char*)andMaskData.data(), andMaskData.size());
                    if (!iStream) {
                        throw ImageLoadError{fmt::format("Failed to read AND mask of size {}", andMaskData.size())};
                    }

                    vector<Channel*> alphaChannels;
                    for (auto& image : imageData) {
                        auto* const alphaChannel = image.mutableChannel("A");
                        if (!alphaChannel) {
                            tlog::warning() << fmt::format("No alpha channel but AND mask. Skipping AND mask application.", i);
                            continue;
                        }

                        alphaChannels.emplace_back(alphaChannel);
                    }

                    const bool flipVertically = size.y() > 0;
                    co_await ThreadPool::global().parallelForAsync<int>(
                        0,
                        entry.height,
                        (size_t)entry.width * entry.height * alphaChannels.size(),
                        [&](int y) {
                            const size_t rowStart = y * bytesPerRow;
                            const int outputY = flipVertically ? entry.height - 1 - y : y;

                            for (int x = 0; x < entry.width; ++x) {
                                const size_t pixelBit = (size_t)x;
                                const size_t pixelByte = pixelBit / 8;
                                const size_t pixelBitOffset = pixelBit - pixelByte * 8;
                                const bool isTransparent = (andMaskData[rowStart + pixelByte] >> (7 - pixelBitOffset)) & 1;

                                if (isTransparent) {
                                    for (auto* c : alphaChannels) {
                                        c->dynamicSetAt({x, outputY}, 0.0f);
                                    }
                                }
                            }
                        },
                        priority
                    );
                }
            } catch (const FormatNotSupported&) {
                tlog::warning() << fmt::format("Neither a PNG nor a BMP image", i);
                continue;
            } catch (const ImageLoadError& e) {
                tlog::warning() << fmt::format("Malformed BMP image: {}", i, e.what());
                continue;
            }
        }

        for (auto& image : imageData) {
            image.partName = Channel::joinIfNonempty(fmt::format("images.{}", i), image.partName);
        }

        result.insert(result.end(), make_move_iterator(imageData.begin()), make_move_iterator(imageData.end()));
    }

    co_return result;
}

} // namespace tev
