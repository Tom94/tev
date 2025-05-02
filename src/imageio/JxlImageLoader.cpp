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

#include <jxl/encode.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Chroma.h>
#include <tev/imageio/JxlImageLoader.h>

#include <jxl/cms.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <istream>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Helper to identify JPEG XL files by signature
bool isJxlImage(istream& iStream) {
    uint8_t signature[16];
    iStream.read(reinterpret_cast<char*>(signature), sizeof(signature));

    bool result = false;
    if (!!iStream && iStream.gcount() == sizeof(signature)) {
        auto sig = JxlSignatureCheck(signature, sizeof(signature));
        result = (sig == JxlSignature::JXL_SIG_CODESTREAM || sig == JxlSignature::JXL_SIG_CONTAINER);
    }

    iStream.clear();
    iStream.seekg(0);
    return result;
}
} // namespace

Task<vector<ImageData>> JxlImageLoader::load(istream& iStream, const fs::path& path, const string& channelSelector, int priority, bool) const {
    if (!isJxlImage(iStream)) {
        throw FormatNotSupportedException{"File is not a JPEG XL image."};
    }

    // Read entire file into memory
    iStream.seekg(0, ios::end);
    size_t fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    vector<uint8_t> fileData(fileSize);
    iStream.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    if (!iStream || static_cast<size_t>(iStream.gcount()) != fileSize) {
        throw runtime_error{"Failed to read file data."};
    }

    // Set up jxl decoder
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw runtime_error{"Failed to create decoder."};
    }

    auto runner = JxlThreadParallelRunnerMake(nullptr, thread::hardware_concurrency());
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw runtime_error{"Failed to set parallel runner for decoder."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE)) {
        throw runtime_error{"Failed to subscribe to decoder events."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw runtime_error{"Failed to set input for decoder."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetCms(decoder.get(), *JxlGetDefaultCms())) {
        throw runtime_error{"Failed to set CMS for decoder."};
    }

    // State that gets updated during various decoding steps. Is reused for each frame of an animated image to avoid reallocation.
    JxlBasicInfo info;
    JxlPixelFormat format;
    JxlColorEncoding colorEncoding;
    vector<float> pixels;
    bool hasAlpha = false;
    bool hasAlphaPremultiplied = false;

    vector<ImageData> result;
    vector<uint8_t> iccProfile;

    // Decode the image
    while (true) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        switch (status) {
            default: break; // Ignore other status codes
            case JXL_DEC_SUCCESS: co_return result;
            case JXL_DEC_ERROR: throw runtime_error{"Error decoding image."};
            case JXL_DEC_NEED_MORE_INPUT: throw runtime_error{"Incomplete image data."};
            case JXL_DEC_BASIC_INFO: {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &info)) {
                    throw runtime_error{"Failed to get basic info from image."};
                }

                if (info.num_color_channels > 3) {
                    throw runtime_error{fmt::format("More than 3 color channels ({}) are not supported.", info.num_color_channels)};
                }

                hasAlpha = info.alpha_bits > 0;
                hasAlphaPremultiplied = info.alpha_premultiplied > 0;

                if (hasAlpha && info.num_extra_channels == 0) {
                    throw runtime_error{"Image has alpha channel, but no extra channels."};
                }

                // libjxl expects the alpha channels to be decoded as part of the image (despite counting as an extra channel)
                // and all other extra channels to be decoded as separate channels.
                uint32_t numChannels = info.num_color_channels + (hasAlpha ? 1 : 0);
                format = {numChannels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                break;
            }
            case JXL_DEC_COLOR_ENCODING: {
                size_t size = 0;
                if (JxlDecoderGetICCProfileSize(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &size) !=
                    JXL_DEC_SUCCESS) {
                    throw runtime_error{"Failed to get ICC profile size from image."};
                }

                iccProfile.resize(size);
                if (JxlDecoderGetColorAsICCProfile(
                        decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, iccProfile.data(), size
                    ) != JXL_DEC_SUCCESS) {
                    throw runtime_error{"Failed to get ICC profile from image."};
                }

                if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &colorEncoding) !=
                    JXL_DEC_SUCCESS) {
                    tlog::debug() << "Failed to get color encoding from image. Relying purely on ICC profile.";
                } else {
                    if (colorEncoding.color_space == JxlColorSpace::JXL_COLOR_SPACE_XYB) {
                        tlog::warning() << "Image has XYB color space. This might be broken.";
                        JxlColorEncodingSetToLinearSRGB(&colorEncoding, false /* XYB is never grayscale */);

                        // Clear the ICC profile and instead rely on the jxl decoder to give us linear sRGB
                        iccProfile.clear();
                    }

                    // Encourage the decoder to use the specified color profile, even if it is the one we just read
                    if (JxlDecoderSetPreferredColorProfile(decoder.get(), &colorEncoding) != JXL_DEC_SUCCESS) {
                        throw runtime_error{"Failed to set preferred color profile."};
                    }
                }
                break;
            }
            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                size_t bufferSize;
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(decoder.get(), &format, &bufferSize)) {
                    throw runtime_error{"Failed to get output buffer size."};
                }

                pixels.resize(bufferSize / sizeof(float));
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), bufferSize)) {
                    throw runtime_error{"Failed to set output buffer."};
                }

                status = JxlDecoderProcessInput(decoder.get());
                if (status == JXL_DEC_ERROR) {
                    std::cout << "Failed to process input: " << std::endl;
                }

                result.emplace_back();
                ImageData& data = result.back();

                Vector2i size{(int)info.xsize, (int)info.ysize};

                int numChannels = format.num_channels;
                data.channels = makeNChannels(numChannels, size);
                data.hasPremultipliedAlpha = hasAlphaPremultiplied;

                bool channelsLoaded = false;
                if (!iccProfile.empty()) {
                    tlog::debug() << "Found ICC color profile. Attempting to apply...";

                    try {
                        co_await convertIccToRec709(
                            iccProfile,
                            size,
                            info.num_color_channels,
                            hasAlpha ? (hasAlphaPremultiplied ? EAlphaKind::Premultiplied : EAlphaKind::Straight) : EAlphaKind::None,
                            pixels.data(),
                            data.channels.front().data(),
                            priority
                        );

                        // The abover conversion outputs straight alpha, even if its input was premultiplied
                        data.hasPremultipliedAlpha = false;

                        channelsLoaded = true;
                    } catch (const exception& e) { throw runtime_error{"Failed to apply ICC color profile: " + string(e.what())}; }
                }

                // If we didn't load the channels via the ICC profile, we need to do it manually. We'll assume the image is already in the
                // colorspace tev expects: linear sRGB Rec.709.
                if (!channelsLoaded) {
                    const size_t nSamplesPerRow = size.x() * numChannels;
                    co_await ThreadPool::global().parallelForAsync<size_t>(
                        0,
                        size.y(),
                        [&](size_t y) {
                            size_t srcOffset = y * nSamplesPerRow;
                            for (int x = 0; x < size.x(); ++x) {
                                size_t baseIdx = srcOffset + x * numChannels;
                                for (int c = 0; c < numChannels; ++c) {
                                    data.channels[c].at({x, (int)y}) = pixels[baseIdx + c];
                                }
                            }
                        },
                        priority
                    );
                }
            } break;
        }
    }

    co_return result;
}

} // namespace tev
