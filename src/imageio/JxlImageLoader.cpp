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
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
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

string jxlToString(JxlPrimaries type) {
    switch (type) {
        case JXL_PRIMARIES_SRGB: return "srgb";
        case JXL_PRIMARIES_CUSTOM: return "custom";
        case JXL_PRIMARIES_2100: return "bt2100";
        case JXL_PRIMARIES_P3: return "p3";
    }

    return "invalid";
};

string jxlToString(JxlWhitePoint type) {
    switch (type) {
        case JXL_WHITE_POINT_D65: return "d65";
        case JXL_WHITE_POINT_CUSTOM: return "custom";
        case JXL_WHITE_POINT_E: return "e";
        case JXL_WHITE_POINT_DCI: return "dci";
    }

    return "invalid";
};

string jxlToString(JxlTransferFunction type) {
    switch (type) {
        case JXL_TRANSFER_FUNCTION_709: return "709";
        case JXL_TRANSFER_FUNCTION_UNKNOWN: return "unknown";
        case JXL_TRANSFER_FUNCTION_LINEAR: return "linear";
        case JXL_TRANSFER_FUNCTION_SRGB: return "srgb";
        case JXL_TRANSFER_FUNCTION_PQ: return "pq";
        case JXL_TRANSFER_FUNCTION_DCI: return "dci";
        case JXL_TRANSFER_FUNCTION_HLG: return "hlg";
        case JXL_TRANSFER_FUNCTION_GAMMA: return "gamma";
    }

    return "invalid";
};

} // namespace

Task<vector<ImageData>> JxlImageLoader::load(istream& iStream, const fs::path& path, string_view channelSelector, int priority, bool) const {
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
        throw ImageLoadError{"Failed to read file data."};
    }

    // Set up jxl decoder
    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw ImageLoadError{"Failed to create decoder."};
    }

    struct RunnerData {
        int priority;
    } runnerData{priority};

    const auto jxlRunParallel = [](void* runnerOpaque,
                                   void* jpegxlOpaque,
                                   JxlParallelRunInit init,
                                   JxlParallelRunFunction func,
                                   uint32_t startRange,
                                   uint32_t endRange) {
        const auto* runnerData = reinterpret_cast<RunnerData*>(runnerOpaque);
        static const uint32_t hardwareConcurrency = std::thread::hardware_concurrency();

        const uint32_t range = endRange - startRange;
        const uint32_t nTasks = std::min({(uint32_t)ThreadPool::global().numThreads(), hardwareConcurrency, range});

        const auto initResult = init(jpegxlOpaque, nTasks);
        if (initResult != 0) {
            return initResult;
        }

        std::vector<Task<void>> tasks;
        for (uint32_t i = 0; i < nTasks; ++i) {
            const uint32_t taskStart = startRange + (range * i / nTasks);
            const uint32_t taskEnd = startRange + (range * (i + 1) / nTasks);
            TEV_ASSERT(taskStart != taskEnd, "Should not produce tasks with empty range.");

            tasks.emplace_back(
                [](JxlParallelRunFunction func, void* jpegxlOpaque, uint32_t taskId, uint32_t tStart, uint32_t tEnd, int tPriority, ThreadPool* pool
                ) -> Task<void> {
                    co_await pool->enqueueCoroutine(tPriority);
                    for (uint32_t j = tStart; j < tEnd; ++j) {
                        func(jpegxlOpaque, j, taskId);
                    }
                }(func, jpegxlOpaque, i, taskStart, taskEnd, runnerData->priority, &ThreadPool::global())
            );
        }

        // This is janky, because it doesn't follow the coroutine paradigm. But it is the only way to get the thread pool to cooperate
        // with the JXL API that expects a non-coroutine function here. We will offload the JxlImageLoader::load() function into a
        // wholly separate thread to avoid blocking the thread pool as a consequence.
        waitAll<Task<void>>(tasks);
        return 0;
    };

    // Since we allow the JXL decoder to run in our coroutine thread pool, despite being not a coroutine itself and thus having to wait on
    // each task's completion, we need to offload the governing code (i.e. this function) to a separate thread to avoid stalling the
    // threadpool. We achieve this by temporarily adding an extra thread to the thread pool until JXL is done.
    ThreadPool::global().startThreads(1);
    const ScopeGuard threadPoolGuard{[] { ThreadPool::global().shutdownThreads(1); }};

    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), jxlRunParallel, &runnerData)) {
        throw ImageLoadError{"Failed to set parallel runner for decoder."};
    }

    if (JXL_DEC_SUCCESS !=
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME)) {
        throw ImageLoadError{"Failed to subscribe to decoder events."};
    }

    // "Boxes" contain all kinds of metadata, such as exif, which we'll want to get at. By default, they are compressed via Brotli,
    // making them inaccessible to typical exif libraries. Hence: set them to get decompressed during decoding.
    if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE)) {
        throw ImageLoadError{"Failed to set decompress boxes."};
    }

    // We expressly don't want the decoder to unpremultiply the alpha channel for us, because this becomes a more and more lossy
    // operation the more emissive a color is (i.e. as color/alpha approaches inf). If it turns out that the color profile isn't linaer,
    // we will be forced to unmultiply the alpha channel due to an idiosyncracy in the jxl format where alpha multiplication is defined
    // in non-linear space.
    if (JXL_DEC_SUCCESS != JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_FALSE)) {
        throw ImageLoadError{"Failed to set unpremultiply alpha."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw ImageLoadError{"Failed to set input for decoder."};
    }

    // State that gets updated during various decoding steps. Is reused for each frame of an animated image to avoid reallocation.
    JxlBasicInfo info;
    vector<float> colorData;

    size_t frameCount = 0;
    string frameName;

    vector<ImageData> result;
    vector<uint8_t> iccProfile;

    optional<JxlColorEncoding> ce = nullopt;

    // Decode the image
    while (true) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        switch (status) {
            default: throw ImageLoadError{fmt::format("Unknown decoder status: {}", (size_t)status)};
            case JXL_DEC_SUCCESS: goto l_decode_success;
            case JXL_DEC_ERROR: throw ImageLoadError{"Error decoding image."};
            case JXL_DEC_NEED_MORE_INPUT: throw ImageLoadError{"Incomplete image data."};
            case JXL_DEC_BASIC_INFO: {
                if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &info)) {
                    throw ImageLoadError{"Failed to get basic info from image."};
                }

                if (info.num_color_channels > 3) {
                    throw ImageLoadError{fmt::format("More than 3 color channels ({}) are not supported.", info.num_color_channels)};
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
                    throw ImageLoadError{"Image has alpha channel, but no extra channels."};
                }
            } break;
            case JXL_DEC_COLOR_ENCODING: {
                // JxlColorEncoding takes precedence over ICC profiles if it is available. While ICC presence is always guaranteed,
                // JxlColorEncoding has certain advantages in the presence of HDR transfer functions, such as PQ or HLG, so we cannot rely
                // only on the ICC profile for color space conversion.
                //
                // Technically, the Jxl decoder can directly convert to linear sRGB in many cases but we observed artifacts in some of our
                // test images when it did so, so we will do the color space conversion ourselves as long as the color space isn't XYB which
                // we do not support yet.
                const auto target = JXL_COLOR_PROFILE_TARGET_DATA;
                if (JxlColorEncoding localCe; JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(decoder.get(), target, &localCe)) {
                    iccProfile.clear();
                    if (localCe.color_space == JXL_COLOR_SPACE_XYB) {
                        ce = nullopt;
                        JxlColorEncodingSetToLinearSRGB(&ce.value(), false /* XYB is never grayscale */);
                        if (JxlDecoderSetPreferredColorProfile(decoder.get(), &ce.value()) != JXL_DEC_SUCCESS) {
                            throw ImageLoadError{"Failed to set up XYB->sRGB conversion."};
                        }
                    } else {
                        ce = localCe;
                    }
                } else {
                    // The jxl spec says that color space can *always* unambiguously be determined from an ICC color encoding, so we can
                    // rely on being able to get the ICC profile from the decoder if all else fails.
                    size_t size = 0;
                    if (JXL_DEC_SUCCESS != JxlDecoderGetICCProfileSize(decoder.get(), target, &size)) {
                        throw ImageLoadError{"Failed to get ICC profile size from image."};
                    }

                    iccProfile.resize(size);
                    if (JXL_DEC_SUCCESS != JxlDecoderGetColorAsICCProfile(decoder.get(), target, iccProfile.data(), size)) {
                        throw ImageLoadError{"Failed to get ICC profile from image."};
                    }
                }
            } break;
            case JXL_DEC_FRAME: {
                const size_t frameId = frameCount++;

                JxlFrameHeader frameHeader;
                if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(decoder.get(), &frameHeader)) {
                    throw ImageLoadError{"Failed to get frame header."};
                }

                if (frameHeader.name_length == 0) {
                    frameName = fmt::format("frames.{}", frameId);
                } else {
                    frameName.resize(frameHeader.name_length + 1); // +1 for null terminator
                    if (JXL_DEC_SUCCESS != JxlDecoderGetFrameName(decoder.get(), frameName.data(), frameName.size() + 1)) {
                        throw ImageLoadError{"Failed to get frame name."};
                    }
                }

                tlog::debug(
                ) << fmt::format("Frame {}: duration={}, is_last={} name={}", frameId, frameHeader.duration, frameHeader.is_last, frameName);
            } break;
            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                // libjxl expects the alpha channels to be decoded as part of the image (despite counting as an extra channel) and all
                // other extra channels to be decoded as separate channels.
                int numColorChannels = info.num_color_channels + (info.alpha_bits ? 1 : 0);
                int numExtraChannels = info.num_extra_channels;

                size_t bufferSize;

                // Main image buffer & decode setup
                JxlPixelFormat imageFormat = {(uint32_t)numColorChannels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(decoder.get(), &imageFormat, &bufferSize)) {
                    throw ImageLoadError{"Failed to get output buffer size."};
                }

                colorData.resize(bufferSize / sizeof(float));
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &imageFormat, colorData.data(), bufferSize)) {
                    throw ImageLoadError{"Failed to set output buffer."};
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
                        throw ImageLoadError{fmt::format("Failed to get extra channel {}'s info.", i)};
                    }

                    extraChannel.dimShift = extraChannelInfo.dim_shift;
                    if (extraChannelInfo.name_length == 0) {
                        extraChannel.name = fmt::format("extra.{}.{}", i, jxlToString(extraChannelInfo.type));
                    } else {
                        extraChannel.name.resize(extraChannelInfo.name_length + 1); // +1 for null terminator
                        if (JXL_DEC_SUCCESS !=
                            JxlDecoderGetExtraChannelName(decoder.get(), i, extraChannel.name.data(), extraChannel.name.size() + 1)) {
                            throw ImageLoadError{fmt::format("Failed to get extra channel {}'s name.", i)};
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
                        throw ImageLoadError{fmt::format("Failed to get extra channel {}'s buffer size.", i)};
                    }

                    extraChannel.data.resize(bufferSize / sizeof(float));
                    if (JXL_DEC_SUCCESS !=
                        JxlDecoderSetExtraChannelBuffer(decoder.get(), &imageFormat, extraChannel.data.data(), bufferSize, i)) {
                        throw ImageLoadError{fmt::format("Failed to set extra channel {}'s buffer.", i)};
                    }

                    extraChannel.active = true;
                }

                status = JxlDecoderProcessInput(decoder.get());
                if (status == JXL_DEC_ERROR) {
                    throw ImageLoadError{"Error processing input."};
                }

                ImageData& data = result.emplace_back();

                Vector2i size{(int)info.xsize, (int)info.ysize};

                data.channels = makeRgbaInterleavedChannels(numColorChannels, info.alpha_bits, size);
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
                            info.alpha_bits ? (info.alpha_premultiplied ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) :
                                              EAlphaKind::None,
                            EPixelFormat::F32,
                            (uint8_t*)colorData.data(),
                            data.channels.front().data(),
                            priority
                        );

                        data.hasPremultipliedAlpha = true;

                        colorChannelsLoaded = true;
                    } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
                }

                // If we didn't load the channels via the ICC profile, we need to load them manually.
                if (!colorChannelsLoaded) {
                    co_await toFloat32(
                        (float*)colorData.data(), numColorChannels, data.channels.front().data(), 4, size, info.alpha_bits, priority
                    );

                    // If color encoding information is available, we need to use it to convert to linear sRGB. Otherwise, assume the
                    // decoder has already prepared the data in linear sRGB for us.
                    if (ce) {
                        tlog::debug() << fmt::format(
                            "JxlColorEncoding: colorspace={}, primaries={}, whitepoint={}, transfer={}",
                            jxlToString(ce->color_space),
                            jxlToString(ce->primaries),
                            jxlToString(ce->white_point),
                            jxlToString(ce->transfer_function)
                        );

                        // Primaries are only valid for RGB data. We need to set up a conversion matrix only if we aren't already in sRGB.
                        if (ce->color_space == JXL_COLOR_SPACE_RGB) {
                            if (ce->primaries != JXL_PRIMARIES_SRGB || ce->white_point != JXL_WHITE_POINT_D65) {
                                data.toRec709 = chromaToRec709Matrix({
                                    {{(float)ce->primaries_red_xy[0], (float)ce->primaries_red_xy[1]},
                                     {(float)ce->primaries_green_xy[0], (float)ce->primaries_green_xy[1]},
                                     {(float)ce->primaries_blue_xy[0], (float)ce->primaries_blue_xy[1]},
                                     {(float)ce->white_point_xy[0], (float)ce->white_point_xy[1]}}
                                });
                            }
                        }

                        const bool hasGamma = ce->transfer_function == JXL_TRANSFER_FUNCTION_GAMMA;
                        if (hasGamma) {
                            tlog::debug() << fmt::format("gamma={}", ce->gamma);
                        }

                        auto cicpTransfer = hasGamma ? ituth273::ETransferCharacteristics::Unspecified :
                                                       static_cast<ituth273::ETransferCharacteristics>(ce->transfer_function);

                        if (!hasGamma && !ituth273::isTransferImplemented(cicpTransfer)) {
                            tlog::warning() << fmt::format("Unsupported transfer '{}'. Using sRGB instead.", ituth273::toString(cicpTransfer));
                            cicpTransfer = ituth273::ETransferCharacteristics::SRGB;
                        }

                        auto* pixelData = data.channels.front().data();
                        const size_t numPixels = size.x() * (size_t)size.y();
                        co_await ThreadPool::global().parallelForAsync<size_t>(
                            0,
                            numPixels,
                            [&](size_t i) {
                                // Jxl unfortunately premultiplies the alpha channel in non-linear space (after application of the
                                // transfer), so we must unpremultiply prior to the color space conversion and transfer function inversion.
                                // See https://github.com/libjxl/conformance/issues/39#issuecomment-3004735767
                                const float alpha = info.alpha_bits ? pixelData[i * 4 + 3] : 1.0f;
                                const float factor = info.alpha_premultiplied && alpha > 0.0001f ? (1.0f / alpha) : 1.0f;
                                const float invFactor = info.alpha_premultiplied && alpha > 0.0001f ? alpha : 1.0f;

                                for (size_t c = 0; c < 3; ++c) {
                                    const float val = pixelData[i * 4 + c];
                                    if (hasGamma) {
                                        pixelData[i * 4 + c] = invFactor * std::pow(factor * val, 1.0f / (float)ce->gamma);
                                    } else {
                                        pixelData[i * 4 + c] = invFactor * ituth273::invTransfer(cicpTransfer, factor * val);
                                    }
                                }
                            },
                            priority
                        );
                    }
                }

                // Load and upscale extra channels if present
                for (size_t i = 0; i < extraChannels.size(); ++i) {
                    const ExtraChannelInfo& extraChannel = extraChannels[i];
                    if (!extraChannel.active) {
                        continue;
                    }

                    // Resize the channel to the image size
                    auto& channel = data.channels.emplace_back(Channel{extraChannel.name, size});
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
l_decode_success:

    JxlDecoderRewind(decoder.get());
    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        tlog::warning() << "Failed to set input for second decoder pass.";
        co_return result;
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BOX)) {
        tlog::warning() << "Failed to subscribe to box events.";
        co_return result;
    }

    // Attempt to get metadata in a separate pass. The reason we perform a separate pass is that box decoding appears to interfere with
    // regular image decoding behavior.
    JxlDecoderStatus status = JXL_DEC_SUCCESS;
    while (true) {
        status = JxlDecoderProcessInput(decoder.get());
        if (status != JXL_DEC_BOX) {
            break;
        }

        JxlBoxType type = {};
        if (JXL_DEC_SUCCESS != JxlDecoderGetBoxType(decoder.get(), type, JXL_TRUE)) {
            throw ImageLoadError{"Failed to get box type."};
        }

        if (type == "Exif"s) {
            tlog::debug() << "Found EXIF metadata. Attempting to load...";

            // 1 KiB should be enough for most exif data. If not, we'll dynamically resize as we keep decoding. We can't get the precise
            // size ahead of time, because the decoder doesn't know how large the box will be until it has been fully decoded.
            vector<uint8_t> exifData(1024);
            if (JXL_DEC_SUCCESS != JxlDecoderSetBoxBuffer(decoder.get(), exifData.data(), exifData.size())) {
                throw ImageLoadError{"Failed to set initial box buffer."};
            }

            status = JxlDecoderProcessInput(decoder.get());
            while (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
                tlog::debug() << fmt::format("Doubling box buffer size from {} to {} bytes.", exifData.size(), exifData.size() * 2);
                if (JXL_DEC_SUCCESS != JxlDecoderReleaseBoxBuffer(decoder.get())) {
                    throw ImageLoadError{"Failed to release box buffer for resize."};
                }

                exifData.resize(exifData.size() * 2);
                if (JXL_DEC_SUCCESS != JxlDecoderSetBoxBuffer(decoder.get(), exifData.data(), exifData.size())) {
                    throw ImageLoadError{"Failed to set resized box buffer."};
                }

                status = JxlDecoderProcessInput(decoder.get());
            }

            if (status != JXL_DEC_SUCCESS && status != JXL_DEC_BOX) {
                throw ImageLoadError{"Failed to process box."};
            }

            try {
                if (exifData.size() < 4) {
                    throw invalid_argument{"Invalid EXIF data: box size is smaller than 4 bytes."};
                }

                uint32_t offset = *(uint32_t*)exifData.data();
                if (endian::native != endian::big) {
                    offset = swapBytes(offset);
                }

                if (offset + 4 > exifData.size()) {
                    throw invalid_argument{"Invalid EXIF data: offset is larger than box size."};
                }

                tlog::debug() << fmt::format("EXIF data offset: {}", offset);
                exifData.erase(exifData.begin(), exifData.begin() + 4 + offset);

                auto exif = Exif{exifData};
                auto exifAttributes = exif.toAttributes();

                for (auto&& data : result) {
                    data.attributes.emplace_back(exifAttributes);
                }
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to parse exif data: {}", e.what()); }
        }
    }

    if (status != JXL_DEC_SUCCESS && status != JXL_DEC_NEED_MORE_INPUT) {
        tlog::warning() << "Unexpected decoder status after processing boxes: " << (size_t)status;
    }

    co_return result;
}

} // namespace tev
