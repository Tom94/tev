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

#include <tev/ThreadPool.h>
#include <tev/imageio/JxlImageLoader.h>
#include <tev/imageio/Chroma.h>

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
        const size_t signatureSize = 128;
        vector<uint8_t> signature(signatureSize);

        // Save current stream position
        auto startPos = iStream.tellg();
        
        // Read potential signature bytes
        iStream.read(reinterpret_cast<char*>(signature.data()), signatureSize);

        // Check if we got a valid signature
        bool result = false;
        if (!!iStream && iStream.gcount() == signatureSize) {
            auto sig = JxlSignatureCheck(signature.data(), signature.size());
            result = (sig == JxlSignature::JXL_SIG_CODESTREAM || sig == JxlSignature::JXL_SIG_CONTAINER);
        }
        
        // Reset stream position
        iStream.clear();
        iStream.seekg(startPos);
        
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
        throw runtime_error{"Failed to read JPEG XL file data."};
    }

    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw runtime_error{"Failed to create JPEG XL decoder."};
    }

    // Set up multi-threaded runner
    auto runner = JxlThreadParallelRunnerMake(nullptr, 16);//ThreadPool::global().threadCount());
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw runtime_error{"Failed to set parallel runner for JPEG XL decoder."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE)) {
        throw runtime_error{"Failed to subscribe to JPEG XL decoder events."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw runtime_error{"Failed to set input for JPEG XL decoder."};
    }

    JxlBasicInfo info;
    JxlPixelFormat format;
    vector<float> pixels;
    bool hasAlpha = false;
    Matrix4f toRec709 = Matrix4f(1.0);

    // Create image data
    vector<ImageData> result;

    // Process the image
    JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
    while (status != JXL_DEC_SUCCESS) {
        if (status == JXL_DEC_ERROR) {
            throw runtime_error{"Error decoding JPEG XL image."};
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            throw runtime_error{"Incomplete JPEG XL image data."};
        } else if (status == JXL_DEC_BASIC_INFO) {
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &info)) {
                throw runtime_error{"Failed to get basic info from JPEG XL image."};
            }
            
            hasAlpha = info.alpha_bits > 0;
            format = {info.num_color_channels + info.num_extra_channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
            status = JxlDecoderProcessInput(decoder.get());
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            // Get color profile information
            JxlColorEncoding color_encoding;
            if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA, &color_encoding)) {
                // If the image is not already in sRGB/Rec709, we may need to convert
                if (color_encoding.white_point != JXL_WHITE_POINT_D65 || 
                    color_encoding.primaries != JXL_PRIMARIES_SRGB) {
                    // This is a simplified approach - for a full implementation 
                    // we'd need proper color space conversion based on the specific primaries
                    if (color_encoding.primaries == JXL_PRIMARIES_CUSTOM) {
                        // Get custom primaries if available
                        float primaries[8];
                        size_t primaries_size = sizeof(primaries);
                        if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsICCProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_ORIGINAL, nullptr, primaries_size)) {
                            // Set up conversion matrix - simplified approach
                            // In a real implementation, we'd need to properly extract the primaries
                            // from the ICC profile and calculate an accurate conversion matrix
                            toRec709 = Matrix4f(1.0); // Placeholder
                        }
                    }
                }
            }
            
            status = JxlDecoderProcessInput(decoder.get());
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t buffer_size;
            if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(decoder.get(), &format, &buffer_size)) {
                throw runtime_error{"Failed to get output buffer size for JPEG XL image."};
            }

            pixels.resize(buffer_size / sizeof(float));
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &format, pixels.data(), buffer_size)) {
                throw runtime_error{"Failed to set output buffer for JPEG XL image."};
            }

            // Read the pixel data into the pixels buffer
            status = JxlDecoderProcessInput(decoder.get());

            // Check if we got any pixels
            if (pixels.empty()) {
                throw runtime_error{"Failed to decode JPEG XL image data."};
            }

            result.emplace_back();
            ImageData& data = result.back();

            // Set dimensions
            Vector2i size{(int)info.xsize, (int)info.ysize};
            data.dataWindow = {
                {0,        0       },
                {size.x(), size.y()}
            };
            data.displayWindow = data.dataWindow;
            data.hasPremultipliedAlpha = false;

            // Set color transform if needed
            if (toRec709 != Matrix4f(1.0)) {
                data.toRec709 = toRec709;
            }

            // Create channels
            int numChannels = format.num_channels;
            vector<Channel> channels = makeNChannels(numChannels, size);

            // Check if the channel selector matches any of our channels
            bool matchFound = false;
            for (int i = 0; i < numChannels; ++i) {
                if (matchesFuzzy(channels[i].name(), channelSelector)) {
                    matchFound = true;
                    break;
                }
            }

            if (!matchFound && !channelSelector.empty()) {
                throw invalid_argument{fmt::format("No channels match '{}'.", channelSelector)};
            }

            // Copy pixels to channels
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                [&](int y) {
                    for (int x = 0; x < size.x(); ++x) {
                        size_t pixelIdx = ((size_t)y * size.x() + x) * numChannels; // RGBA format from JXL decoder

                        for (int c = 0; c < numChannels; ++c) {
                            channels[c].at({x, y}) = pixels[pixelIdx + c];
                        }
                    }
                },
                priority
            );

            // Add channels to result if they match the selector
            for (int i = 0; i < numChannels; ++i) {
                if (channelSelector.empty() || matchesFuzzy(channels[i].name(), channelSelector)) {
                    data.channels.emplace_back(std::move(channels[i]));
                }
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            // We've got the full image
            status = JxlDecoderProcessInput(decoder.get());
        } else {
            // Handle other events or skip them
            status = JxlDecoderProcessInput(decoder.get());
        }
    }

    co_return result;
}

} // namespace tev
