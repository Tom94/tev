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

#include "tev/imageio/ImageLoader.h"
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/JxlImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/gain_map.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <istream>
#include <limits>
#include <span>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Helper to identify JPEG XL files by signature
bool isJxlImage(span<const uint8_t> data) {
    if (data.size() < 16) {
        return false;
    }

    const auto sig = JxlSignatureCheck(data.data(), data.size());
    return sig == JxlSignature::JXL_SIG_CODESTREAM || sig == JxlSignature::JXL_SIG_CONTAINER;
}

bool isJxlImage(istream& iStream) {
    uint8_t signature[16];
    iStream.read(reinterpret_cast<char*>(signature), sizeof(signature));

    const bool result = !!iStream && iStream.gcount() == sizeof(signature) && isJxlImage(signature);

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

Task<vector<ImageData>> JxlImageLoader::load(
    istream& iStream, const fs::path& path, string_view channelSelector, const ImageLoaderSettings& settings, int priority
) const {
    if (!isJxlImage(iStream)) {
        throw FormatNotSupported{"File is not a JPEG XL image."};
    }

    iStream.seekg(0, ios::end);
    const auto fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    HeapArray<uint8_t> fileData(fileSize);
    iStream.read(reinterpret_cast<char*>(fileData.data()), fileSize);

    co_return co_await load(fileData, path, channelSelector, settings, priority, false);
}

Task<vector<ImageData>> JxlImageLoader::load(
    span<const uint8_t> fileData,
    const fs::path& path,
    string_view channelSelector,
    const ImageLoaderSettings& settings,
    int priority,
    bool skipColorProcessing,
    size_t* bitsPerSampleOut,
    EPixelType* pixelTypeOut
) const {
    if (!isJxlImage(fileData)) {
        throw FormatNotSupported{"Data is not a JPEG XL image."};
    }

    auto decoder = JxlDecoderMake(nullptr);
    if (!decoder) {
        throw ImageLoadError{"Failed to create decoder."};
    }

    struct RunnerData {
        int priority;
        uint32_t jxlSuggestedNumThreads;
    } runnerData{
        .priority = priority,
        .jxlSuggestedNumThreads = thread::hardware_concurrency(),
    };

    const auto jxlRunParallel =
        [](void* runnerOpaque, void* jpegxlOpaque, JxlParallelRunInit init, JxlParallelRunFunction func, uint32_t startRange, uint32_t endRange) {
            // We use a separate thread pool from the global one (still a singleton), because the JXL API does not allow jxlRunParallel to be a
            // co-routine. I.e. we need to synchronously wait for the work to finish, which could deadlock the global threadpool. Other mitigation
            // strategies involve temporarily creating and removing extra threads from the global threadpool (which tev previously implemented),
            // but this approach here scales better to huge numbers of images (n-cores extra threads instead of n-images extra threads).
            static auto jxlPool = ThreadPool();

            const auto* runnerData = reinterpret_cast<RunnerData*>(runnerOpaque);

            const uint32_t range = endRange - startRange;
            const uint32_t numTasks = std::min(
                jxlPool.nTasks<uint32_t>(
                    0,
                    range,
                    numeric_limits<uint32_t>::max() // Max parallelism up to range tasks & hardware concurrency
                ),
                runnerData->jxlSuggestedNumThreads // ...or fewer threads if JXL suggests so
            );

            const auto initResult = init(jpegxlOpaque, numTasks);
            if (initResult != 0) {
                return initResult;
            }

            jxlPool
                .parallelForAsync<uint32_t>(
                    0,
                    numTasks,
                    numeric_limits<uint32_t>::max(), // Maximum parallelism up to numTasks threads
                    [&](uint32_t i) {
                        const uint32_t taskStart = startRange + (range * i / numTasks);
                        const uint32_t taskEnd = startRange + (range * (i + 1) / numTasks);
                        TEV_ASSERT(taskStart != taskEnd, "Should not produce tasks with empty range.");

                        for (uint32_t j = taskStart; j < taskEnd; ++j) {
                            func(jpegxlOpaque, j, (uint32_t)i);
                        }
                    },
                    runnerData->priority
                )
                // The synchronous parallel for loop is janky, because it doesn't follow the coroutine paradigm. But it is the only way to
                // get the thread pool to cooperate with the JXL API that expects a non-coroutine function here. We will offload the
                // JxlImageLoader::load() function into a wholly separate thread to avoid blocking the thread pool as a consequence.
                .get();

            return 0;
        };

    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), jxlRunParallel, &runnerData)) {
        throw ImageLoadError{"Failed to set parallel runner for decoder."};
    }

    if (JXL_DEC_SUCCESS !=
        JxlDecoderSubscribeEvents(
            decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE | JXL_DEC_FRAME | JXL_DEC_BOX | JXL_DEC_BOX_COMPLETE
        )) {
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

    // Disable automatic orientation handling. We want to handle orientation ourselves later on because we can do it faster.
    if (JXL_DEC_SUCCESS != JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE)) {
        throw ImageLoadError{"Failed to set keep orientation."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw ImageLoadError{"Failed to set input for decoder."};
    }

    // Tells the decoder that we are done providing input after the initial input buffer. Necessary to properly handle joint subscription of
    // box and image related events. See https://libjxl.readthedocs.io/en/latest/api_decoder.html#_CPPv420JxlDecoderCloseInputP10JxlDecoder
    JxlDecoderCloseInput(decoder.get());

    // State that gets updated during various decoding steps. Is reused for each frame of an animated image to avoid reallocation.
    JxlBasicInfo info;
    HeapArray<float> colorData;
    HeapArray<uint8_t> iccProfile;

    size_t frameCount = 0;
    string frameName;

    vector<ImageData> result;
    vector<AttributeNode> attributes;

    optional<JxlColorEncoding> ce = nullopt;

    struct GainMapInfo {
        IsoGainMapMetadata metadata;
        optional<chroma_t> altChroma;
        ImageData imageData;
    };

    optional<GainMapInfo> gainMapInfo = nullopt;

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
                    "Image size={}x{} channels={} bits_per_sample={}:{} alpha_bits={} alpha_premultiplied={} have_animation={} intensity_target={} orientation={}",
                    info.xsize,
                    info.ysize,
                    info.num_color_channels,
                    info.bits_per_sample,
                    info.exponent_bits_per_sample,
                    info.alpha_bits,
                    info.alpha_premultiplied,
                    info.have_animation,
                    info.intensity_target,
                    toString(static_cast<EOrientation>(info.orientation))
                );

                runnerData.jxlSuggestedNumThreads = std::max(JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize), (uint32_t)1);

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
                    iccProfile = {};
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

                    iccProfile = HeapArray<uint8_t>{size};
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

                tlog::debug() << fmt::format(
                    "Frame {}: duration={} is_last={} name={}", frameId, frameHeader.duration, frameHeader.is_last, frameName
                );
            } break;
            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                // libjxl expects the alpha channels to be decoded as part of the image (despite counting as an extra channel) and all
                // other extra channels to be decoded as separate channels.
                const size_t numChannels = info.num_color_channels + (info.alpha_bits ? 1 : 0);
                const size_t numExtraChannels = info.num_extra_channels;

                size_t bufferSize;

                // Main image buffer & decode setup
                JxlPixelFormat imageFormat = {(uint32_t)numChannels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                if (JXL_DEC_SUCCESS != JxlDecoderImageOutBufferSize(decoder.get(), &imageFormat, &bufferSize)) {
                    throw ImageLoadError{"Failed to get output buffer size."};
                }

                colorData = HeapArray<float>{bufferSize / sizeof(float)};
                if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &imageFormat, colorData.data(), bufferSize)) {
                    throw ImageLoadError{"Failed to set output buffer."};
                }

                ImageData& data = result.emplace_back();
                if (info.have_animation) {
                    data.partName = frameName;
                }

                struct ExtraChannelInfo {
                    string name;
                    HeapArray<float> data;
                    uint32_t bitsPerSample = 0; // bits per sample of the channel
                    uint32_t dimShift = 0; // number of power of 2 stops the channel is downsampled
                    bool active = false;
                };

                // Extra channels buffer & decode setup
                vector<ExtraChannelInfo> extraChannels(numExtraChannels);

                JxlPixelFormat extraChannelFormat = {1, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
                for (size_t i = 0; i < numExtraChannels; ++i) {
                    ExtraChannelInfo& extraChannel = extraChannels[i];

                    JxlExtraChannelInfo extraChannelInfo;
                    if (JXL_DEC_SUCCESS != JxlDecoderGetExtraChannelInfo(decoder.get(), i, &extraChannelInfo)) {
                        throw ImageLoadError{fmt::format("Failed to get extra channel {}'s info.", i)};
                    }

                    extraChannel.bitsPerSample = extraChannelInfo.bits_per_sample;
                    extraChannel.dimShift = extraChannelInfo.dim_shift;
                    if (extraChannelInfo.name_length == 0) {
                        extraChannel.name = fmt::format("extra.{}.{}", i, jxlToString(extraChannelInfo.type));
                    } else {
                        vector<char> channelName(extraChannelInfo.name_length + 1, '\0'); // +1 for null terminator
                        if (JXL_DEC_SUCCESS != JxlDecoderGetExtraChannelName(decoder.get(), i, channelName.data(), channelName.size())) {
                            throw ImageLoadError{fmt::format("Failed to get extra channel {}'s name.", i)};
                        }

                        extraChannel.name = fmt::format("extra.{}", channelName.data());
                    }

                    extraChannel.name = Channel::joinIfNonempty(data.partName, extraChannel.name);

                    // Skip loading of extra channels that don't match the selector entirely. And skip alpha channels, because they're
                    // already part of the color channels.
                    const bool skip = !matchesFuzzy(extraChannel.name, channelSelector) || extraChannelInfo.type == JXL_CHANNEL_ALPHA;
                    if (skip) {
                        continue;
                    }

                    tlog::debug() << fmt::format(
                        "Loading extra channel #{}: name={} bits_per_sample={} dim_shift={}",
                        i,
                        extraChannel.name,
                        extraChannel.bitsPerSample,
                        extraChannel.dimShift
                    );

                    if (JXL_DEC_SUCCESS != JxlDecoderExtraChannelBufferSize(decoder.get(), &extraChannelFormat, &bufferSize, i)) {
                        throw ImageLoadError{fmt::format("Failed to get extra channel {}'s buffer size.", i)};
                    }

                    extraChannel.data = HeapArray<float>{bufferSize / sizeof(float)};
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

                const Vector2i size{(int)info.xsize, (int)info.ysize};

                const int numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
                data.channels = co_await makeRgbaInterleavedChannels(
                    numChannels,
                    numInterleavedChannels,
                    info.alpha_bits,
                    size,
                    EPixelFormat::F32,
                    info.bits_per_sample > 16 ? EPixelFormat::F32 : EPixelFormat::F16,
                    data.partName,
                    priority
                );

                const auto inView = MultiChannelView<float>{colorData.data(), numChannels, size};
                const auto outView = MultiChannelView<float>{data.channels};

                // If there's no alpha channel, treat as premultiplied (by 1)
                data.hasPremultipliedAlpha = info.alpha_bits == 0 || info.alpha_premultiplied;

                // JXL's orientation values match EXIF orientation tags (which also match our EOrientation enum).
                data.orientation = (EOrientation)info.orientation;

                if (info.intensity_target != 0 && info.intensity_target != 255) {
                    // Some JXL files use the intensity_target field to indicate maxCLL (e.g. https://people.csail.mit.edu/ericchan/hdr/).
                    // Values of 0 and 255 are reserved/meaningless, so ignore those.
                    data.hdrMetadata.maxCLL = info.intensity_target;
                }

                if (bitsPerSampleOut) {
                    *bitsPerSampleOut = info.bits_per_sample;
                }

                if (pixelTypeOut) {
                    *pixelTypeOut = info.exponent_bits_per_sample > 0 ? EPixelType::Float : EPixelType::Uint;
                }

                bool colorChannelsLoaded = false;
                if (iccProfile && !skipColorProcessing) {
                    tlog::debug() << "Found ICC color profile. Attempting to apply...";

                    try {
                        const auto profile = ColorProfile::fromIcc(iccProfile);
                        co_await toLinearSrgbPremul(
                            profile,
                            info.alpha_bits ? (info.alpha_premultiplied ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) :
                                              EAlphaKind::None,
                            inView,
                            outView,
                            nullopt,
                            priority
                        );

                        data.hasPremultipliedAlpha = true;
                        data.readMetadataFromIcc(profile);

                        colorChannelsLoaded = true;
                    } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
                }

                // If we didn't load the channels via the ICC profile, we need to load them manually.
                if (!colorChannelsLoaded) {
                    co_await toFloat32(colorData.data(), numChannels, outView, info.alpha_bits, priority);

                    // If color encoding information is available, we need to use it to convert to linear sRGB. Otherwise, assume the
                    // decoder has already prepared the data in linear sRGB for us.
                    if (ce && !skipColorProcessing) {
                        data.renderingIntent = static_cast<ERenderingIntent>(ce->rendering_intent);

                        tlog::debug() << fmt::format(
                            "JxlColorEncoding: colorspace={} primaries={} whitepoint={} transfer={} intent={}",
                            jxlToString(ce->color_space),
                            jxlToString(ce->primaries),
                            jxlToString(ce->white_point),
                            jxlToString(ce->transfer_function),
                            toString(data.renderingIntent)
                        );

                        // Primaries are only valid for RGB data. We need to set up a conversion matrix only if we aren't already in sRGB.
                        if (ce->color_space == JXL_COLOR_SPACE_RGB) {
                            if (ce->primaries != JXL_PRIMARIES_SRGB || ce->white_point != JXL_WHITE_POINT_D65) {
                                const chroma_t chroma = {
                                    {{(float)ce->primaries_red_xy[0], (float)ce->primaries_red_xy[1]},
                                     {(float)ce->primaries_green_xy[0], (float)ce->primaries_green_xy[1]},
                                     {(float)ce->primaries_blue_xy[0], (float)ce->primaries_blue_xy[1]},
                                     {(float)ce->white_point_xy[0], (float)ce->white_point_xy[1]}}
                                };

                                data.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), data.renderingIntent);
                                data.nativeMetadata.chroma = chroma;
                            } else {
                                data.nativeMetadata.chroma = rec709Chroma();
                            }
                        }

                        const bool hasGamma = ce->transfer_function == JXL_TRANSFER_FUNCTION_GAMMA;
                        if (hasGamma) {
                            tlog::debug() << fmt::format("gamma={}", ce->gamma);
                            data.nativeMetadata.gamma = (float)ce->gamma;
                        }

                        auto cicpTransfer = hasGamma ? ituth273::ETransfer::GenericGamma :
                                                       static_cast<ituth273::ETransfer>(ce->transfer_function);

                        if (!hasGamma) {
                            if (!ituth273::isTransferImplemented(cicpTransfer)) {
                                tlog::warning()
                                    << fmt::format("Unsupported transfer '{}'. Using sRGB instead.", ituth273::toString(cicpTransfer));
                                cicpTransfer = ituth273::ETransfer::SRGB;
                            }
                        }

                        data.nativeMetadata.transfer = cicpTransfer;
                        data.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicpTransfer);

                        const size_t numPixels = size.x() * (size_t)size.y();
                        co_await ThreadPool::global().parallelForAsync<size_t>(
                            0,
                            numPixels,
                            numPixels * numInterleavedChannels,
                            [&](size_t i) {
                                // Jxl unfortunately premultiplies the alpha channel in non-linear space (after application of the
                                // transfer), so we must unpremultiply prior to the color space conversion and transfer function inversion.
                                // See https://github.com/libjxl/conformance/issues/39#issuecomment-3004735767
                                const float alpha = info.alpha_bits ? outView[-1, i] : 1.0f;
                                const float factor = info.alpha_premultiplied && alpha > 0.0001f ? (1.0f / alpha) : 1.0f;
                                const float invFactor = info.alpha_premultiplied && alpha > 0.0001f ? alpha : 1.0f;

                                Vector3f color;
                                for (uint32_t c = 0; c < info.num_color_channels; ++c) {
                                    color[c] = outView[c, i];
                                }

                                if (hasGamma) {
                                    color = pow(color * factor, 1.0f / (float)ce->gamma) * invFactor;
                                } else {
                                    color = ituth273::invTransfer(cicpTransfer, color * factor) * invFactor;
                                }

                                for (uint32_t c = 0; c < info.num_color_channels; ++c) {
                                    outView[c, i] = color[c];
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

                    auto& channel = data.channels.emplace_back(
                        extraChannel.name, size, EPixelFormat::F32, extraChannel.bitsPerSample > 16 ? EPixelFormat::F32 : EPixelFormat::F16
                    );
                    const auto view = channel.view<float>();

                    const size_t numPixels = (size_t)size.x() * size.y();
                    co_await ThreadPool::global().parallelForAsync<int>(
                        0,
                        size.y(),
                        numPixels,
                        [&](int y) {
                            const size_t srcOffset = (size_t)y * (size.x() >> extraChannel.dimShift);
                            for (int x = 0; x < size.x(); ++x) {
                                view[x, y] = extraChannel.data[srcOffset + (x >> extraChannel.dimShift)];
                            }
                        },
                        priority
                    );
                }
            } break;
            case JXL_DEC_BOX: {
                JxlBoxType type = {};
                if (JXL_DEC_SUCCESS != JxlDecoderGetBoxType(decoder.get(), type, JXL_TRUE)) {
                    throw ImageLoadError{"Failed to get box type."};
                }

                const auto boxTypeStr = toUpper(trim(string_view{type, 4}));
                if (boxTypeStr != "EXIF" && boxTypeStr != "XML" && boxTypeStr != "JHGM") {
                    continue;
                }

                tlog::debug() << fmt::format("Found metadata box of type {}.", boxTypeStr);

                if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(decoder.get(), JXL_TRUE)) {
                    throw ImageLoadError{"Failed to set decompress boxes."};
                }

                uint64_t boxSize = 0;
                if (JXL_DEC_SUCCESS != JxlDecoderGetBoxSizeContents(decoder.get(), &boxSize)) {
                    throw ImageLoadError{"Failed to get metadata box size."};
                }

                boxSize = std::max(boxSize, (uint64_t)1024); // Start with at least 1 KB buffer
                tlog::debug() << fmt::format("Metadata box initial size: {} bytes.", boxSize);

                HeapArray<uint8_t> metadata(boxSize);
                size_t pos = 0;
                while (true) {
                    const size_t remaining = metadata.size() - pos;
                    if (JXL_DEC_SUCCESS != JxlDecoderSetBoxBuffer(decoder.get(), metadata.data() + pos, remaining)) {
                        throw ImageLoadError{"Failed to set box buffer."};
                    }

                    status = JxlDecoderProcessInput(decoder.get());
                    if (status != JXL_DEC_BOX_COMPLETE && status != JXL_DEC_BOX_NEED_MORE_OUTPUT) {
                        throw ImageLoadError{fmt::format("Failed to process box: {}", (size_t)status)};
                    }

                    const size_t notYetWritten = JxlDecoderReleaseBoxBuffer(decoder.get());
                    if (notYetWritten > remaining) {
                        throw ImageLoadError{"Decoder reported more data than buffer size."};
                    }

                    pos += remaining - notYetWritten;
                    if (status == JXL_DEC_BOX_COMPLETE || status == JXL_DEC_SUCCESS) {
                        tlog::debug()
                            << fmt::format("Completed reading box of type {} ({} bytes / {} size)", boxTypeStr, pos, metadata.size());
                        metadata.resize(pos);
                        break;
                    } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
                        metadata.resize(metadata.size() * 2); // Double buffer size and try again
                        tlog::debug() << fmt::format("Doubled box buffer size to {}", metadata.size());
                    } else {
                        throw ImageLoadError{"Unexpected decoder status when reading box."};
                    }
                }

                if (boxTypeStr == "XML") {
                    try {
                        const Xmp xmp{
                            string_view{(const char*)metadata.data(), metadata.size()}
                        };

                        attributes.emplace_back(xmp.attributes());
                    } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to parse XMP data: {}", e.what()); }
                } else if (boxTypeStr == "EXIF") {
                    try {
                        if (metadata.size() < 4) {
                            throw invalid_argument{"Invalid EXIF data: box size is smaller than 4 bytes."};
                        }

                        uint32_t offset = *(uint32_t*)metadata.data();
                        if (endian::native != endian::big) {
                            offset = swapBytes(offset);
                        }

                        if (offset > metadata.size() - 4) {
                            throw invalid_argument{"Invalid EXIF data: offset is larger than box size."};
                        }

                        tlog::debug() << fmt::format("EXIF data offset: {}", offset);

                        const auto exif = Exif{span<uint8_t>{metadata}.subspan(4 + offset)};
                        const auto exifAttributes = exif.toAttributes();

                        attributes.emplace_back(exifAttributes);
                    } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to parse EXIF data: {}", e.what()); }
                } else if (boxTypeStr == "JHGM") {
                    if (JxlGainMapBundle jgmb; JxlGainMapReadBundle(&jgmb, metadata.data(), metadata.size(), nullptr)) {
                        tlog::debug() << fmt::format("Parsed JHGM gain map box: version={}", jgmb.jhgm_version);

                        try {
                            if (jgmb.gain_map_size == 0 || jgmb.gain_map == nullptr) {
                                throw invalid_argument{"Gain map image data is missing."};
                            }

                            if (jgmb.gain_map_metadata_size == 0 || jgmb.gain_map_metadata == nullptr) {
                                throw invalid_argument{"Gain map metadata is missing."};
                            }

                            const auto isoGainMapMetadata = IsoGainMapMetadata{
                                {jgmb.gain_map_metadata, jgmb.gain_map_metadata_size}
                            };
                            attributes.emplace_back(isoGainMapMetadata.toAttributes());

                            optional<chroma_t> altChroma = nullopt;
                            if (jgmb.has_color_encoding) {
                                tlog::debug() << "Gain map has JxlColorEncoding. Parsing...";

                                const auto& jce = jgmb.color_encoding;

                                if (jce.color_space != JXL_COLOR_SPACE_RGB) {
                                    throw invalid_argument{"Gain map color encoding is not RGB."};
                                }

                                altChroma = chroma_t{
                                    {{(float)jce.primaries_red_xy[0], (float)jce.primaries_red_xy[1]},
                                     {(float)jce.primaries_green_xy[0], (float)jce.primaries_green_xy[1]},
                                     {(float)jce.primaries_blue_xy[0], (float)jce.primaries_blue_xy[1]},
                                     {(float)jce.white_point_xy[0], (float)jce.white_point_xy[1]}}
                                };
                            } else if (jgmb.alt_icc_size > 0 && jgmb.alt_icc != nullptr) {
                                tlog::debug() << "Gain map has alternative ICC profile. Attempting to parse...";

                                try {
                                    const auto profile = ColorProfile::fromIcc({jgmb.alt_icc, jgmb.alt_icc_size});
                                    altChroma = profile.chroma();
                                } catch (const runtime_error& e) {
                                    tlog::warning() << fmt::format("Failed to parse gain map alternative ICC profile: {}", e.what());
                                }
                            }

                            try {
                                auto gainMapLoadResult = co_await load(
                                    span<const uint8_t>{jgmb.gain_map, jgmb.gain_map_size}, path, channelSelector, settings, priority, true
                                );

                                tlog::debug() << fmt::format("Decoded JXL gain map image data into {} image(s).", gainMapLoadResult.size());

                                if (gainMapLoadResult.size() == 0 || gainMapLoadResult.front().channels.empty()) {
                                    throw invalid_argument{"Decoded gain map image data is empty."};
                                } else if (gainMapLoadResult.size() > 1) {
                                    tlog::warning() << "Decoded gain map image data contains multiple images. Using the first one.";
                                }

                                auto& gainMap = gainMapLoadResult.front();
                                for (auto& channel : gainMap.channels) {
                                    channel.setName(Channel::joinIfNonempty("gainmap", channel.name()));
                                }

                                gainMapInfo = GainMapInfo{
                                    .metadata = std::move(isoGainMapMetadata),
                                    .altChroma = altChroma,
                                    .imageData = std::move(gainMap),
                                };
                            } catch (const ImageLoadError& e) {
                                throw invalid_argument{fmt::format("Failed to decode gain map image data: {}", e.what())};
                            }
                        } catch (const invalid_argument& e) {
                            tlog::warning() << fmt::format("Failed to load ISO 21496-1 gain map from JHGM box: {}", e.what());
                        }
                    } else {
                        tlog::warning() << "Failed to parse JHGM gain map box.";
                    }
                } else {
                    tlog::warning() << fmt::format("Unhandled box type: {}", boxTypeStr);
                }
            } break;
            case JXL_DEC_BOX_COMPLETE: {
                tlog::debug() << "Completed processing box.";
            } break;
        }
    }
l_decode_success:

    // Attach collected attributes to all image data parts
    for (auto&& data : result) {
        data.attributes.insert(data.attributes.end(), attributes.begin(), attributes.end());

        if (gainMapInfo) {
            auto& gainMap = gainMapInfo->imageData;
            co_await preprocessAndApplyIsoGainMap(
                data, gainMap, gainMapInfo->metadata, data.nativeMetadata.chroma, gainMapInfo->altChroma, settings.gainmapHeadroom, priority
            );

            data.channels.insert(data.channels.end(), make_move_iterator(gainMap.channels.begin()), make_move_iterator(gainMap.channels.end()));
        }
    }

    co_return result;
}

} // namespace tev
