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
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/PfmImageLoader.h>

#include <bit>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

enum class PamType : uint8_t {
    P1 = 1,
    P2,
    P3,
    P4,
    P5,
    P6,
    P7,
    Pf,
    PF,
    PF4,
};

static string_view toString(const PamType pamType) {
    switch (pamType) {
        case PamType::P1: return "P1";
        case PamType::P2: return "P2";
        case PamType::P3: return "P3";
        case PamType::P4: return "P4";
        case PamType::P5: return "P5";
        case PamType::P6: return "P6";
        case PamType::P7: return "P7";
        case PamType::Pf: return "Pf";
        case PamType::PF: return "PF";
        case PamType::PF4: return "PF4";
        default: throw runtime_error{"Unknown pam type."};
    }
}

static optional<PamType> pamReadHeader(istream& iStream) {
    char pf[3];
    iStream.read(pf, 3);

    if (!iStream || pf[0] != 'P') {
        return nullopt;
    }

    if (pf[1] >= '1' && pf[1] <= '7') {
        return (PamType)(pf[1] - '0');
    } else if (pf[1] == 'F') {
        if (pf[2] == '4') {
            return PamType::PF4;
        } else {
            return PamType::PF;
        }
    } else if (pf[1] == 'f') {
        return PamType::Pf;
    }

    return nullopt;
}

static bool isPam(const PamType pamType) { return (uint8_t)pamType <= 7; }

static bool isPfm(const PamType pamType) { return (uint8_t)pamType > 7; }

static uint8_t version(const PamType pamType) {
    if (!isPam(pamType)) {
        throw ImageLoadError{"Pam type has no version"};
    }

    return (uint8_t)pamType;
}

Task<vector<ImageData>> PfmImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    size_t frameIdx = 0;

    const auto loadPam = [&iStream, &frameIdx, priority]() -> Task<vector<ImageData>> {
        PamType pamType;
        if (auto result = pamReadHeader(iStream)) {
            pamType = *result;
        } else {
            throw FormatNotSupported{"Invalid PFM/PAM magic string."};
        }

        AttributeNode header;
        header.name = "PAM header";

        AttributeNode& global = header.children.emplace_back();
        global.name = "Global";

        global.children.emplace_back(
            AttributeNode{
                .name = "Format",
                .value = string{toString(pamType)},
                .type = "string",
                .children = {},
            }
        );

        ostringstream comment;
        Vector2i size = {0, 0};
        float scale = 1.0f;
        size_t bitsPerChannel = 0;
        bool isLittleEndian = false;
        size_t numChannels = 0;

        const bool pfm = isPfm(pamType), pam = isPam(pamType);
        const int ver = pam ? version(pamType) : -1;

        const bool isBinary = pfm || ver >= 4;
        const size_t numHeaderParams = pfm ? 3 : (ver == 1 || ver == 4) ? 2 : 3;
        if (pfm || ver != 7) {
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

            // Skip until end of line after reading header parameters. This is unfortunately a bit messy because some images might start
            // their data right after the last header parameter without a newline... in which case the following code is incorrect if the
            // beginning of the image data is equivalent to a whitespace character.
            while (isspace(iStream.peek())) {
                iStream.get();
            }

            if (headerParams.size() < numHeaderParams) {
                throw ImageLoadError{"Not enough header parameters."};
            }

            TEV_ASSERT(headerParams.size() >= 2, "Not enough header parameters.");

            try {
                size.x() = stoi(headerParams[0]);
                size.y() = stoi(headerParams[1]);
            } catch (const invalid_argument&) {
                throw ImageLoadError{fmt::format("Invalid image size '{} {}'", headerParams[0], headerParams[1])};
            } catch (const out_of_range&) {
                throw ImageLoadError{fmt::format("Image size '{} {}' out of range", headerParams[0], headerParams[1])};
            }

            if (pfm) {
                TEV_ASSERT(headerParams.size() >= 3, "No scale parameter in PFM header.");

                try {
                    scale = stof(headerParams[2]);
                } catch (const invalid_argument&) {
                    throw ImageLoadError{fmt::format("Invalid scale '{}'", headerParams[2])};
                } catch (const out_of_range&) { throw ImageLoadError{fmt::format("Scale '{}' out of range", headerParams[2])}; }

                isLittleEndian = scale < 0;
                scale = abs(scale);
                bitsPerChannel = 32;

                switch (pamType) {
                    case PamType::Pf: numChannels = 1; break;
                    case PamType::PF: numChannels = 3; break;
                    case PamType::PF4: numChannels = 4; break;
                    default: TEV_ASSERT(false, "Invalid pfm type");
                }
            } else {
                numChannels = (ver == 3 || ver == 6) ? 3 : 1;

                // Bitmap uses just 1 bit per channel. Otherwise set depending on max value to either 8, 16, or 32.
                if (ver == 1 || ver == 4) {
                    bitsPerChannel = 1;
                    scale = 1.0f;
                } else {
                    TEV_ASSERT(headerParams.size() >= 3, "No max value parameter in PNM header.");

                    try {
                        const unsigned long long maxVal = stoull(headerParams[2]); // Maxval
                        bitsPerChannel = maxVal >= (1 << 16) ? 32 : maxVal >= (1 << 8) ? 16 : 8;
                        scale = 1.0f / maxVal;
                    } catch (const invalid_argument&) {
                        throw ImageLoadError{fmt::format("Invalid maxval '{}'", headerParams[2])};
                    } catch (const out_of_range&) { throw ImageLoadError{fmt::format("Maxval '{}' out of range", headerParams[2])}; }
                }
            }
        } else {
            // PAM headers are slightly different from pfm/pnm headers. Luckily, they enforce newlines rather than equating them with
            // spaces, making parsing a bit easier.
            string line;
            while (getline(iStream, line)) {
                const auto parts = splitWhitespace(line);
                if (parts.empty()) {
                    continue;
                }

                for (size_t i = 0; i < parts.size(); ++i) {
                    if (parts[i].starts_with("#")) {
                        if (parts[i].size() > 1) {
                            comment << parts[i].substr(1) << ' ';
                        }

                        comment << join(span(parts).subspan(i + 1), " ") << '\n';
                    }
                }

                const auto key = parts.at(0);
                if (key.starts_with("#")) {
                    continue;
                }

                if (key == "ENDHDR") {
                    break;
                }

                if (parts.size() < 2) {
                    tlog::warning() << fmt::format("Missing value of PAM header key '{}'", key);
                    continue;
                }

                const auto value = parts.at(1);

                if (key == "TUPLTYPE") {
                    global.children.emplace_back(
                        AttributeNode{
                            .name = "Tuple type",
                            .value = string{value},
                            .type = "string",
                            .children = {},
                        }
                    );
                } else {
                    try {
                        const unsigned long long ullVal = stoull(string{value});
                        if (key == "WIDTH") {
                            size.x() = (int)ullVal;
                        } else if (key == "HEIGHT") {
                            size.y() = (int)ullVal;
                        } else if (key == "DEPTH") {
                            numChannels = (int)ullVal;
                        } else if (key == "MAXVAL") {
                            const auto maxVal = ullVal;
                            scale = 1.0f / maxVal;
                            bitsPerChannel = maxVal >= (1 << 16) ? 32 : maxVal >= (1 << 8) ? 16 : 8;
                        } else {
                            tlog::warning() << fmt::format("Invalid PAM key '{}'", key);
                        }
                    } catch (const invalid_argument&) {
                        throw ImageLoadError{fmt::format("Invalid {}: '{}'", key, value)};
                    } catch (const out_of_range&) { throw ImageLoadError{fmt::format("{}'s value '{}' is out of range", key, value)}; }
                }
            }
        }

        const string commentStr = comment.str();
        if (!commentStr.empty()) {
            global.children.emplace_back(
                AttributeNode{
                    .name = "Comment",
                    .value = string{trim(commentStr)},
                    .type = "string",
                    .children = {},
                }
            );
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
            toString(pamType),
            size.x(),
            size.y(),
            numChannels,
            bitsPerChannel,
            scale,
            isLittleEndian ? "little" : "big"
        );

        vector<ImageData> result(1);
        ImageData& resultData = result.front();
        resultData.partName = fmt::format("frames.{}", frameIdx++);

        if (!global.children.empty()) {
            resultData.attributes.emplace_back(std::move(header));
        }

        const EPixelFormat desiredFormat = bitsPerChannel == 32 ? EPixelFormat::F32 : EPixelFormat::F16;

        const size_t numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
        const bool hasAlpha = numChannels == 2 || numChannels == 4;
        resultData.channels = co_await makeRgbaInterleavedChannels(
            numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, desiredFormat, resultData.partName, priority
        );

        const auto numSamplesPerRow = (size_t)size.x() * numChannels;
        const auto numSamples = numSamplesPerRow * size.y();

        // ASCII formats (<= P3) are more easily handled by reading them as if they were 32 bit.
        // The desired format (above) should depend on the actually required bits per channel, though.
        if (ver <= 3) {
            bitsPerChannel = 32;
        }

        // +7 to round up bits to full bytes. Per pbm spec, rows are individually padded
        const auto numBytesPerRow = (numSamplesPerRow * bitsPerChannel + 7) / 8;
        const auto numBytes = numBytesPerRow * size.y();

        const auto dataPtr = make_unique<uint8_t[]>(numBytes);
        uint8_t* const data = dataPtr.get();

        if (isBinary) {
            iStream.read(reinterpret_cast<char*>(data), numBytes);
            if (iStream.gcount() < (streamsize)numBytes) {
                throw ImageLoadError{fmt::format("Insufficient bytes to read ({} vs {})", iStream.gcount(), numBytes)};
            }
        } else {
            TEV_ASSERT(bitsPerChannel == 32, "ASCII PNM with non-32 bits per channel not supported.");
            TEV_ASSERT(ver >= 1 && ver <= 3, "ASCII PNM with invalid version.");

            uint32_t* const uintData = reinterpret_cast<uint32_t*>(data);

            if (ver == 1) {
                // Special case for P1 bitmaps where spaces are optional and the only allowed values are 0 (white) and 1 (black)
                char c;
                size_t i = 0;
                while (iStream.get(c) && i < numSamples) {
                    if (c == '0' || c == '1') {
                        uintData[i++] = c == '0' ? 1 : 0;
                    } else {
                        continue;
                    }
                }
            } else {
                // P2 and P3 contain regular, space/newline-separated integers
                for (size_t i = 0; i < numSamples; ++i) {
                    if (!iStream) {
                        throw ImageLoadError{"Unexpected end of file in ASCII PNM data."};
                    }

                    iStream >> uintData[i];
                }
            }
        }

        // Reverse bytes of every entry if endianness does not match up with system
        const bool shallSwapBytes = isBinary && (endian::native == endian::little) != isLittleEndian;

        if (pfm) {
            const float* const floatData = reinterpret_cast<float*>(data);
            co_await ThreadPool::global().parallelForAsync(
                0,
                size.y(),
                numSamples,
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
                            resultData.channels[c].dynamicSetAt({x, size.y() - (int)y - 1}, scale * val);
                        }
                    }
                },
                priority
            );
        } else {
            if (bitsPerChannel == 32) {
                if (shallSwapBytes) {
                    co_await ThreadPool::global().parallelForAsync<size_t>(
                        0, numSamples, numSamples, [&](size_t i) { ((uint32_t*)data)[i] = swapBytes(((uint32_t*)data)[i]); }, priority
                    );
                }

                co_await toFloat32<uint32_t, true>(
                    (const uint32_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
                );
            } else if (bitsPerChannel == 16) {
                if (shallSwapBytes) {
                    co_await ThreadPool::global().parallelForAsync<size_t>(
                        0, numSamples, numSamples, [&](size_t i) { ((uint16_t*)data)[i] = swapBytes(((uint16_t*)data)[i]); }, priority
                    );
                }

                co_await toFloat32<uint16_t, true>(
                    (const uint16_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
                );
            } else if (bitsPerChannel == 8) {
                co_await toFloat32<uint8_t, true>(
                    (const uint8_t*)data, numChannels, resultData.channels.front().floatData(), numInterleavedChannels, size, hasAlpha, priority, scale
                );
            } else if (bitsPerChannel == 1) {
                co_await ThreadPool::global().parallelForAsync(
                    0,
                    size.y(),
                    numSamples,
                    [&](const int y) {
                        const size_t baseByteIdx = numBytesPerRow * y;

                        for (size_t b = 0; b < numBytesPerRow; ++b) {
                            const uint8_t byte = data[baseByteIdx + b];

                            for (int bitIdx = 0; bitIdx < 8; ++bitIdx) {
                                const size_t sampleIdx = b * 8 + bitIdx;
                                if (sampleIdx >= numSamplesPerRow) {
                                    break;
                                }

                                const size_t x = sampleIdx / numChannels;
                                const size_t c = sampleIdx - x * numChannels;

                                const bool bit = (byte & (1 << (7 - bitIdx))) != 0;
                                resultData.channels[c].dynamicSetAt({(int)x, y}, bit ? scale * 0.0f : scale * 1.0f);
                            }
                        }
                    },
                    priority
                );
            } else {
                throw ImageLoadError{fmt::format("Unsupported bits per channel: {}", bitsPerChannel)};
            }
        }

        // PFM treated like EXR: raw floating point data is scene-referred by default. Usually corresponds to linear light, so should not
        // get its white point adjusted. PNM/PAM, because gamma corrected, like PNG/JPEG, treated as display referred and thus adjusted to
        // the display white point.
        resultData.renderingIntent = pfm ? ERenderingIntent::AbsoluteColorimetric : ERenderingIntent::RelativeColorimetric;

        resultData.nativeMetadata.transfer = pfm ? ituth273::ETransfer::Linear : ituth273::ETransfer::SRGB;
        resultData.nativeMetadata.chroma = rec709Chroma();

        resultData.hasPremultipliedAlpha = !hasAlpha;

        co_return result;
    };

    vector<ImageData> result;

    try {
        while (iStream && !iStream.eof()) {
            auto images = co_await loadPam();
            result.insert(result.end(), make_move_iterator(images.begin()), make_move_iterator(images.end()));
        }
    } catch (const FormatNotSupported& e) {
        if (result.empty()) {
            throw;
        }
    } catch (const ImageLoadError& e) {
        if (result.empty()) {
            throw;
        }
    }

    if (result.size() == 1) {
        // No need for frame names if there's only one frame. Unfortunately, there is no way to tell ahead of time whether a PAM image has
        // multiple frames.
        result.front().partName = "";
    }

    co_return result;
}

} // namespace tev
