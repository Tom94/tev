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
#include <tev/ThreadPool.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/PfmImageLoader.h>

#include <bit>
#include <sstream>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> PfmImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
    char pf[2];
    iStream.read(pf, 2);

    const bool isPfm = iStream && pf[0] == 'P' && (pf[1] == 'F' || pf[1] == 'f');
    const bool isPam = iStream && pf[0] == 'P' && (pf[1] >= '1' && pf[1] <= '7');
    const int pamVersion = isPam ? (pf[1] - '0') : -1;

    if (!isPfm && !isPam) {
        throw FormatNotSupported{"Invalid PFM/PAM magic string."};
    }

    iStream.clear();
    iStream.seekg(0);

    string magic;
    iStream >> magic;

    ostringstream comment;
    Vector2i size = {0, 0};
    float scale = 1.0f;
    size_t bitsPerChannel = 0;
    bool isLittleEndian = false;
    size_t numChannels = 0;

    const bool isBinary = isPfm || pamVersion >= 4;
    const size_t numHeaderParams = isPfm ? 3 : (pamVersion == 1 || pamVersion == 4) ? 2 : 3;
    if (pamVersion != 7) {
        vector<string> headerParams;
        while (iStream && headerParams.size() < numHeaderParams) {
            string part;
            iStream >> part;
            if (part.empty()) {
                continue;
            }

            if (part[0] == '#') {
                string line;
                getline(iStream, line);
                comment << trim(part.substr(1) + line) << '\n';
                continue;
            }

            headerParams.emplace_back(std::move(part));
        }

        // Skip until end of line after reading header parameters. This is unfortunately a bit messy because some images might start their
        // data right after the last header parameter without a newline... in which case the following code is incorrect if the beginning of
        // the image data is equivalent to a whitespace character.
        while (std::isspace(iStream.peek())) {
            iStream.get();
        }

        if (headerParams.size() < numHeaderParams) {
            throw ImageLoadError{"Not enough header parameters."};
        }

        TEV_ASSERT(headerParams.size() >= 2, "Not enough header parameters.");
        size.x() = stoi(headerParams[0]);
        size.y() = stoi(headerParams[1]);

        if (isPfm) {
            TEV_ASSERT(headerParams.size() >= 3, "No scale parameter in PFM header.");
            scale = stof(headerParams[2]);
            isLittleEndian = scale < 0;
            scale = abs(scale);
            bitsPerChannel = 32;

            if (magic == "Pf") {
                numChannels = 1;
            } else if (magic == "PF") {
                numChannels = 3;
            } else if (magic == "PF4") {
                numChannels = 4;
            } else {
                throw FormatNotSupported{fmt::format("Invalid PFM magic string {}", magic)};
            }
        } else {
            numChannels = (pamVersion == 3 || pamVersion == 6) ? 3 : 1;

            // Read scale if available
            unsigned long long maxVal = 1;
            if (pamVersion != 1 && pamVersion != 4) {
                TEV_ASSERT(headerParams.size() >= 3, "No scale parameter in PNM header.");
                const unsigned long long maxVal = stoull(headerParams[2]); // Maxval
                scale = 1.0f / maxVal;
            }

            // ASCII formats (<= P3) are always 32 bits per channel for easier handling.
            // Binary formats use appropriate bit depths.
            if (pamVersion <= 3) {
                bitsPerChannel = 32;
            } else if (pamVersion == 4) {
                bitsPerChannel = 1;
            } else {
                bitsPerChannel = maxVal >= (1 << 16) ? 32 : maxVal >= (1 << 8) ? 16 : 8;
            }
        }
    } else {
        // PAM headers are slightly different from pfm/pnm headers. Luckily, they enforce newlines rather than equating them with spaces,
        // making parsing a bit easier.
        string line;
        while (getline(iStream, line)) {
            if (line.empty()) {
                continue;
            }

            if (line[0] == '#') {
                comment << trim(line.substr(1)) << '\n';
                continue;
            }

            istringstream lineStream{line};
            string key;
            lineStream >> key;

            if (key == "WIDTH") {
                lineStream >> size.x();
            } else if (key == "HEIGHT") {
                lineStream >> size.y();
            } else if (key == "DEPTH") {
                lineStream >> numChannels;
            } else if (key == "MAXVAL") {
                size_t maxVal;
                lineStream >> maxVal;
                scale = 1.0f / maxVal;
                bitsPerChannel = maxVal >= (1 << 16) ? 32 : maxVal >= (1 << 8) ? 16 : 8;
            } else if (key == "TUPLETYPE") {
                // Can be ignored. tev infers interpretation from depth and maxval
            } else if (key == "ENDHDR") {
                break;
            }
        }
    }

    if (!isfinite(scale) || scale == 0) {
        throw ImageLoadError{fmt::format("Invalid scale {}", scale)};
    }

    if (size.x() <= 0 || size.y() <= 0) {
        throw ImageLoadError{fmt::format("Invalid image size {}x{}", size.x(), size.y())};
    }

    if (numChannels <= 0 || numChannels > 4) {
        throw ImageLoadError{fmt::format("Invalid number of channels {}", numChannels)};
    }

    if (bitsPerChannel != 1 && bitsPerChannel != 8 && bitsPerChannel != 16 && bitsPerChannel != 32) {
        throw ImageLoadError{fmt::format("Unsupported bits per channel: {}", bitsPerChannel)};
    }

    tlog::debug() << fmt::format(
        "Loading {} image: size={}x{} channels={} bitsPerChannel={} scale={} endian={}",
        magic,
        size.x(),
        size.y(),
        numChannels,
        bitsPerChannel,
        scale,
        isLittleEndian ? "little" : "big"
    );

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    const EPixelFormat desiredFormat = (isPfm || bitsPerChannel == 32) ? EPixelFormat::F32 : EPixelFormat::F16;

    const size_t numInterleavedChannels = numChannels == 1 ? 1 : 4;
    const bool hasAlpha = numChannels == 2 || numChannels == 4;
    resultData.channels = numInterleavedChannels == 1 ?
        makeNChannels(numChannels, size, EPixelFormat::F32, desiredFormat) :
        makeRgbaInterleavedChannels(numChannels, hasAlpha, size, EPixelFormat::F32, desiredFormat);

    const auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    const auto numEntries = numPixels * numChannels;
    const auto numBytes = numEntries * bitsPerChannel / 8;

    unique_ptr<uint8_t[]> dataPtr{new uint8_t[numBytes]};
    uint8_t* const data = dataPtr.get();

    if (isBinary) {
        iStream.read(reinterpret_cast<char*>(data), numBytes);
        if (iStream.gcount() < (streamsize)numBytes) {
            throw ImageLoadError{fmt::format("Insufficient bytes to read ({} vs {})", iStream.gcount(), numBytes)};
        }
    } else {
        TEV_ASSERT(bitsPerChannel == 32, "ASCII PNM with non-32 bits per channel not supported.");
        TEV_ASSERT(pamVersion >= 1 && pamVersion <= 3, "ASCII PNM with invalid version.");

        uint32_t* const uintData = reinterpret_cast<uint32_t*>(data);

        if (pamVersion == 1) {
            // Special case for P1 bitmaps where spaces are optional and the only allowed values are 0 (white) and 1 (black)
            char c;
            size_t i = 0;
            while (iStream.get(c) && i < numEntries) {
                if (c == '0' || c == '1') {
                    uintData[i++] = c == '0' ? 1 : 0;
                } else {
                    continue;
                }
            }
        } else {
            // P2 and P3 contain regular, space/newline-separated integers
            for (size_t i = 0; i < numEntries; ++i) {
                if (!iStream) {
                    throw ImageLoadError{"Unexpected end of file in ASCII PNM data."};
                }

                iStream >> uintData[i];
            }
        }
    }

    // Reverse bytes of every entry if endianness does not match up with system
    const bool shallSwapBytes = (std::endian::native == std::endian::little) != isLittleEndian;

    if (isPfm) {
        const float* const floatData = reinterpret_cast<float*>(data);
        co_await ThreadPool::global().parallelForAsync(
            0,
            size.y(),
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    const int baseIdx = (y * size.x() + x) * numChannels;
                    for (size_t c = 0; c < numChannels; ++c) {
                        float val = floatData[baseIdx + c];

                        // Thankfully, due to branch prediction, the "if" in the inner loop is no significant overhead.
                        if (shallSwapBytes) {
                            val = swapBytes(val);
                        }

                        // Flip image vertically due to PFM format
                        resultData.channels[c].setAt({x, size.y() - (int)y - 1}, scale * val);
                    }
                }
            },
            priority
        );

        // Treated like EXR: scene-referred by nature. Usually corresponds to linear light, so should not get its white point adjusted.
        resultData.renderingIntent = ERenderingIntent::AbsoluteColorimetric;
    } else {
        if (bitsPerChannel == 32) {
            co_await toFloat32<uint32_t, true>(
                (const uint32_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
            );
        } else if (bitsPerChannel == 16) {
            co_await toFloat32<uint16_t, true>(
                (const uint16_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
            );
        } else if (bitsPerChannel == 8) {
            co_await toFloat32<uint8_t, true>(
                (const uint8_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
            );
        } else if (bitsPerChannel == 1) {
            co_await ThreadPool::global().parallelForAsync(
                (size_t)0,
                numBytes,
                [&](size_t byteIdx) {
                    const uint8_t byte = data[byteIdx];
                    for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
                        const size_t pixelIdx = byteIdx * 8 + bitIdx;
                        if (pixelIdx >= numPixels) {
                            break;
                        }

                        const bool bit = (byte & (1 << (7 - bitIdx))) != 0;
                        resultData.channels[0].setAt(
                            {(int)(pixelIdx % size.x()), (int)(pixelIdx / size.x())}, bit ? scale * 0.0f : scale * 1.0f
                        );
                    }
                },
                priority
            );
        } else {
            throw ImageLoadError{fmt::format("Unsupported bits per channel: {}", bitsPerChannel)};
        }
    }

    resultData.hasPremultipliedAlpha = false;

    const string commentStr = comment.str();
    if (!commentStr.empty()) {
        AttributeNode header;
        header.name = "PAM header";

        AttributeNode& global = header.children.emplace_back();
        global.name = "Global";

        AttributeNode& commentNode = global.children.emplace_back();
        commentNode.name = "Comment";
        commentNode.value = commentStr;
        commentNode.type = "string";

        resultData.attributes.emplace_back(std::move(header));
    }

    co_return result;
}

} // namespace tev
