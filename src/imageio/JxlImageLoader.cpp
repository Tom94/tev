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
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/JxlImageLoader.h>

#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
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

string jxlToString(JxlExtraChannelType type) {
    switch (type) {
        case JXL_CHANNEL_ALPHA: return "alpha";
        case JXL_CHANNEL_DEPTH: return "depth";
        case JXL_CHANNEL_SPOT_COLOR: return "spot_color";
        case JXL_CHANNEL_SELECTION_MASK: return "selection_mask";
        case JXL_CHANNEL_BLACK: return "black";
        case JXL_CHANNEL_CFA: return "cfa";
        case JXL_CHANNEL_THERMAL: return "thermal";
        case JXL_CHANNEL_RESERVED0: return "reserved0";
        case JXL_CHANNEL_RESERVED1: return "reserved1";
        case JXL_CHANNEL_RESERVED2: return "reserved2";
        case JXL_CHANNEL_RESERVED3: return "reserved3";
        case JXL_CHANNEL_RESERVED4: return "reserved4";
        case JXL_CHANNEL_RESERVED5: return "reserved5";
        case JXL_CHANNEL_RESERVED6: return "reserved6";
        case JXL_CHANNEL_RESERVED7: return "reserved7";
        case JXL_CHANNEL_UNKNOWN: return "unknown";
        case JXL_CHANNEL_OPTIONAL: return "optional";
    }

    return "invalid";
};

string jxlToString(JxlColorSpace type) {
    switch (type) {
        case JXL_COLOR_SPACE_GRAY: return "gray";
        case JXL_COLOR_SPACE_RGB: return "rgb";
        case JXL_COLOR_SPACE_XYB: return "xyb";
        case JXL_COLOR_SPACE_UNKNOWN: return "unknown";
    }

    return "invalid";
};

} // namespace

Task<vector<ImageData>> JxlImageLoader::load(istream& iStream, const fs::path& path, const string& channelSelector, int priority, bool) const {
    if (!isJxlImage(iStream)) {
        throw FormatNotSupported{"File is not a JPEG XL image."};
    }

    // Read entire file into memory
    iStream.seekg(0, ios::end);
    size_t fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    vector<uint8_t> fileData(fileSize);
    iStream.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    if (!iStream || static_cast<size_t>(iStream.gcount()) != fileSize) {
        throw LoadError{"Failed to read file data."};
    }

    // Set up jxl decoder
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw LoadError{"Failed to create decoder."};
    }

    auto runner = JxlThreadParallelRunnerMake(nullptr, thread::hardware_concurrency());
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw LoadError{"Failed to set parallel runner for decoder."};
    }

    if (JXL_DEC_SUCCESS !=
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME)) {
        throw LoadError{"Failed to subscribe to decoder events."};
    }

    // We expressly don't want the decoder to unpremultiply the alpha channel for us, because this becomes a more and more lossy operation
    // the more emissive a color is (i.e. as color/alpha approaches inf). If it turns out that the color profile isn't linaer, we will be
    // forced to unmultiply the alpha channel due to an idiosyncracy in the jxl format where alpha multiplication is defined in non-linear
    // space.
    if (JXL_DEC_SUCCESS != JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_FALSE)) {
        throw LoadError{"Failed to set unpremultiply alpha."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw LoadError{"Failed to set input for decoder."};
    }

    // State that gets updated during various decoding steps. Is reused for each frame of an animated image to avoid reallocation.
    JxlBasicInfo info;
    JxlColorSpace colorSpace = JxlColorSpace::JXL_COLOR_SPACE_UNKNOWN;
    vector<float> colorData;

    size_t frameCount = 0;
    string frameName;

    vector<ImageData> result;
    vector<uint8_t> iccProfile;

    // Decode the image
    while (true) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        switch (status) {
            default: break; // Ignore other status codes
            case JXL_DEC_SUCCESS: co_return result;
            case JXL_DEC_ERROR: throw LoadError{"Error decoding image."};
            case JXL_DEC_NEED_MORE_INPUT: throw LoadError{"Incomplete image data."};
            case JXL_DEC_BASIC_INFO: {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &info)) {
                    throw LoadError{"Failed to get basic info from image."};
                }

                if (info.num_color_channels > 3) {
                    throw LoadError{fmt::format("More than 3 color channels ({}) are not supported.", info.num_color_channels)};
                }

                tlog::debug() << fmt::format(
                    "Image size: {}x{}, channels: {}, alpha bits: {}, premultiplied alpha: {}, animation: {}",
                    info.xsize,
                    info.ysize,
                    info.num_color_channels,
                    info.alpha_bits,
                    info.alpha_premultiplied,
                    info.have_animation
                );

                if (info.alpha_bits && info.num_extra_channels == 0) {
                    throw LoadError{"Image has alpha channel, but no extra channels."};
                }

                break;
            }
            case JXL_DEC_COLOR_ENCODING: {
                JxlColorEncoding ce;
                ce.color_space = JxlColorSpace::JXL_COLOR_SPACE_UNKNOWN;

                // libjxl's docs say that the decoder can always convert to linear sRGB if `info.uses_original_profile` is false.
                // Furthermore, the XYB color space can also always be converted to linear sRGB by the decoder. Thus, if either of these is
                // true, leave color space conversion to the decoder.
                //
                // While the docs say that JxlColorEncoding should take precedence over ICC profiles if available, the decoder seems to be
                // unable of correct conversion to linear sRGB in many cases. Thus, we rely on the ICC profile and offload color conversion
                // external cms when the above 2 conditions aren't met.
                bool canDecodeToLinearSRGB = !info.uses_original_profile;
                if (JXL_DEC_SUCCESS ==
                    JxlDecoderGetColorAsEncodedProfile(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &ce)) {
                    canDecodeToLinearSRGB = ce.color_space == JxlColorSpace::JXL_COLOR_SPACE_XYB;
                }

                if (canDecodeToLinearSRGB) {
                    iccProfile.clear();

                    JxlColorEncodingSetToLinearSRGB(&ce, false /* XYB is never grayscale */);
                    if (JxlDecoderSetPreferredColorProfile(decoder.get(), &ce) != JXL_DEC_SUCCESS) {
                        throw LoadError{"Failed to set up XYB->sRGB conversion."};
                    }
                } else {
                    // The jxl spec says that color space can *always* unambiguously be determined from an ICC color encoding, so we can
                    // rely on being able to get the ICC profile from the decoder.
                    size_t size = 0;
                    if (JXL_DEC_SUCCESS !=
                        JxlDecoderGetICCProfileSize(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &size)) {
                        throw LoadError{"Failed to get ICC profile size from image."};
                    }

                    iccProfile.resize(size);
                    if (JXL_DEC_SUCCESS !=
                        JxlDecoderGetColorAsICCProfile(
                            decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, iccProfile.data(), size
                        )) {
                        throw LoadError{"Failed to get ICC profile from image."};
                    }
                }

                colorSpace = ce.color_space;
                tlog::debug() << fmt::format("Image color space: {}", jxlToString(colorSpace));
                break;
            }
            case JXL_DEC_FRAME: {
                size_t frameId = frameCount++;

                JxlFrameHeader frameHeader;
                if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(decoder.get(), &frameHeader)) {
                    throw LoadError{"Failed to get frame header."};
                }

                if (frameHeader.name_length == 0) {
                    frameName = fmt::format("frames.{}", frameId);
                } else {
                    frameName.resize(frameHeader.name_length + 1); // +1 for null terminator
                    if (JXL_DEC_SUCCESS != JxlDecoderGetFrameName(decoder.get(), frameName.data(), frameName.size() + 1)) {
                        throw LoadError{"Failed to get frame name."};
                    }
                }

                tlog::debug(
                ) << fmt::format("Frame {}: duration={}, is_last={} name={}", frameId, frameHeader.duration, frameHeader.is_last, frameName);
                break;
            }
            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                // libjxl expects the alpha channels to be decoded as part of the image (despite counting as an extra channel) and all other
                // extra channels to be decoded as separate channels.
                int numColorChannels = info.num_color_channels + (info.alpha_bits ? 1 : 0);
                int numExtraChannels = info.num_extra_channels;

                size_t bufferSize;

                // Main image buffer & decode setup
                JxlPixelFormat imageFormat = {(uint32_t)numColorChannels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(decoder.get(), &imageFormat, &bufferSize)) {
                    throw LoadError{"Failed to get output buffer size."};
                }

                colorData.resize(bufferSize / sizeof(float));
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &imageFormat, colorData.data(), bufferSize)) {
                    throw LoadError{"Failed to set output buffer."};
                }

                struct ExtraChannelInfo {
                    string name;
                    vector<float> data;
                    uint32_t dimShift = 0; // number of power of 2 stops the channel is downsampled
                    bool active = false;
                };

                // Extra channels buffer & decode setup
                vector<ExtraChannelInfo> extraChannels(numExtraChannels);

                JxlPixelFormat extraChannelFormat = {1, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                for (int i = 0; i < numExtraChannels; ++i) {
                    ExtraChannelInfo& extraChannel = extraChannels[i];

                    JxlExtraChannelInfo extraChannelInfo;
                    if (JXL_DEC_SUCCESS != JxlDecoderGetExtraChannelInfo(decoder.get(), i, &extraChannelInfo)) {
                        throw LoadError{fmt::format("Failed to get extra channel {}'s info.", i)};
                    }

                    extraChannel.dimShift = extraChannelInfo.dim_shift;
                    if (extraChannelInfo.name_length == 0) {
                        extraChannel.name = fmt::format("extra.{}.{}", i, jxlToString(extraChannelInfo.type));
                    } else {
                        extraChannel.name.resize(extraChannelInfo.name_length + 1); // +1 for null terminator
                        if (JXL_DEC_SUCCESS !=
                            JxlDecoderGetExtraChannelName(decoder.get(), i, extraChannel.name.data(), extraChannel.name.size() + 1)) {
                            throw LoadError{fmt::format("Failed to get extra channel {}'s name.", i)};
                        }
                    }

                    // Skip loading of extra channels that don't match the selector entirely. And skip alpha channels, because they're
                    // already part of the color channels.
                    bool skip = !matchesFuzzy(extraChannel.name, channelSelector) || extraChannelInfo.type == JXL_CHANNEL_ALPHA;
                    if (skip) {
                        continue;
                    }

                    tlog::debug() << fmt::format("Loading extra channel {}: {}, dim shift: {}", i, extraChannel.name, extraChannel.dimShift);

                    if (JXL_DEC_SUCCESS != JxlDecoderExtraChannelBufferSize(decoder.get(), &extraChannelFormat, &bufferSize, i)) {
                        throw LoadError{fmt::format("Failed to get extra channel {}'s buffer size.", i)};
                    }

                    extraChannel.data.resize(bufferSize / sizeof(float));
                    if (JXL_DEC_SUCCESS !=
                        JxlDecoderSetExtraChannelBuffer(decoder.get(), &imageFormat, extraChannel.data.data(), bufferSize, i)) {
                        throw LoadError{fmt::format("Failed to set extra channel {}'s buffer.", i)};
                    }

                    extraChannel.active = true;
                }

                status = JxlDecoderProcessInput(decoder.get());
                if (status == JXL_DEC_ERROR) {
                    std::cout << "Failed to process input: " << std::endl;
                }

                result.emplace_back();
                ImageData& data = result.back();

                Vector2i size{(int)info.xsize, (int)info.ysize};

                data.channels = makeNChannels(numColorChannels, size);
                data.hasPremultipliedAlpha = info.alpha_premultiplied;
                if (info.have_animation) {
                    data.partName = frameName;
                }

                bool colorChannelsLoaded = false;
                if (!iccProfile.empty()) {
                    tlog::debug() << "Found ICC color profile. Attempting to apply...";

                    try {
                        co_await toLinearSrgbPremul(
                            ColorProfile::fromIcc(iccProfile.data(), iccProfile.size()),
                            size,
                            info.num_color_channels,
                            info.alpha_bits ? (info.alpha_premultiplied ? EAlphaKind::Premultiplied : EAlphaKind::Straight) : EAlphaKind::None,
                            EPixelFormat::F32,
                            (uint8_t*)colorData.data(),
                            data.channels.front().data(),
                            priority
                        );

                        data.hasPremultipliedAlpha = true;

                        colorChannelsLoaded = true;
                    } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
                }

                // If we didn't load the channels via the ICC profile, we need to do it manually. We'll assume the image is already in the
                // colorspace tev expects: linear sRGB Rec.709.
                if (!colorChannelsLoaded) {
                    co_await toFloat32(
                        (float*)colorData.data(), numColorChannels, data.channels.front().data(), 4, size, info.alpha_bits, priority
                    );
                }

                // Load and upscale extra channels if present
                for (size_t i = 0; i < extraChannels.size(); ++i) {
                    const ExtraChannelInfo& extraChannel = extraChannels[i];
                    if (!extraChannel.active) {
                        continue;
                    }

                    // Resize the channel to the image size
                    data.channels.emplace_back(Channel{extraChannel.name, size});
                    auto& channel = data.channels.back();

                    // Upscale the channel to the image size
                    co_await ThreadPool::global().parallelForAsync<size_t>(
                        0,
                        size.y(),
                        [&](size_t y) {
                            size_t srcOffset = y * (size.x() >> extraChannel.dimShift);
                            for (int x = 0; x < size.x(); ++x) {
                                channel.at({x, (int)y}) = extraChannel.data[srcOffset + (x >> extraChannel.dimShift)];
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
