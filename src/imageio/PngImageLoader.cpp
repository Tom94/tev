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
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/PngImageLoader.h>

#include <png.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> PngImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
    png_byte header[8] = {0};
    iStream.read(reinterpret_cast<char*>(header), 8);
    if (png_sig_cmp(header, 0, 8)) {
        throw FormatNotSupported{"File is not a PNG image."};
    }

    png_structp pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!pngPtr) {
        throw ImageLoadError{"Failed to create PNG read struct."};
    }

    png_infop infoPtr = nullptr;
    ScopeGuard pngGuard{[&]() { png_destroy_read_struct(&pngPtr, &infoPtr, nullptr); }};

    png_set_error_fn(
        pngPtr,
        nullptr,
        [](png_structp, png_const_charp error_msg) { throw ImageLoadError{fmt::format("PNG error: {}", error_msg)}; },
        [](png_structp, png_const_charp warning_msg) { tlog::warning() << fmt::format("PNG warning: {}", warning_msg); }
    );

    infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr) {
        throw ImageLoadError{"Failed to create PNG info struct."};
    }

    png_set_read_fn(pngPtr, &iStream, [](png_structp png_ptr, png_bytep data, png_size_t length) {
        auto stream = reinterpret_cast<istream*>(png_get_io_ptr(png_ptr));
        size_t totalRead = 0;
        while (stream && totalRead < length) {
            stream->read(reinterpret_cast<char*>(data) + totalRead, length - totalRead);
            totalRead += stream->gcount();
        }

        if (totalRead < length) {
            png_error(png_ptr, "Read error");
        }
    });

    // Tell libpng we've already read the signature
    png_set_sig_bytes(pngPtr, 8);

    png_read_info(pngPtr, infoPtr);

    png_uint_32 width, height;
    int bitDepth, colorType, interlaceType;
    png_get_IHDR(pngPtr, infoPtr, &width, &height, &bitDepth, &colorType, &interlaceType, nullptr, nullptr);

    Vector2i size{static_cast<int>(width), static_cast<int>(height)};
    if (size.x() == 0 || size.y() == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    // Determine number of channels
    int numColorChannels = 0;
    int numChannels = 0;
    switch (colorType) {
        case PNG_COLOR_TYPE_GRAY: numColorChannels = numChannels = 1; break;
        case PNG_COLOR_TYPE_GRAY_ALPHA:
            numColorChannels = 1;
            numChannels = 2;
            break;
        case PNG_COLOR_TYPE_RGB: numColorChannels = numChannels = 3; break;
        case PNG_COLOR_TYPE_RGB_ALPHA:
            numColorChannels = 3;
            numChannels = 4;
            break;
        case PNG_COLOR_TYPE_PALETTE:
            png_set_palette_to_rgb(pngPtr);
            numColorChannels = numChannels = 3;
            break;
        default: numColorChannels = numChannels = 0; break;
    }

    if (interlaceType != PNG_INTERLACE_NONE) {
        tlog::debug() << "Image is interlaced. Converting to non-interlaced.";
        png_set_interlace_handling(pngPtr);
    }

    // Only grayscale and palette images can have a bit depth of 1, 2, or 4. Since we configure the PNG reader to convert palette images to
    // RGB, we can additionally configure the reader to convert grayscale images with a bit depth of 1, 2, or 4 to 8-bit and then we only
    // have to deal with either 16-bit or 8-bit images.
    if (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8) {
        tlog::debug() << fmt::format("Converting grayscale image with bit depth {} to 8-bit.", bitDepth);
        png_set_expand_gray_1_2_4_to_8(pngPtr);
    }

    if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS)) {
        tlog::debug() << "Image has transparency channel. Converting to alpha channel.";

        png_set_tRNS_to_alpha(pngPtr); // Convert transparency to alpha channel
        if (numColorChannels != numChannels) {
            throw ImageLoadError{"Image has transparency channel but already has an alpha channel."};
        }

        numChannels += 1;
    }

    if (numColorChannels == 0 || numChannels == 0) {
        throw ImageLoadError{fmt::format("Unsupported PNG color type: {}", colorType)};
    }

    png_read_update_info(pngPtr, infoPtr);

    bitDepth = png_get_bit_depth(pngPtr, infoPtr);
    if (bitDepth != 8 && bitDepth != 16) {
        throw ImageLoadError{fmt::format("Unsupported PNG bit depth: {}", bitDepth)};
    }

    tlog::debug() << fmt::format("PNG image info: size={} numChannels={} bitDepth={} colorType={}", size, numChannels, bitDepth, colorType);

    // 16 bit channels are big endian by default, but we want little endian on little endian systems
    if (bitDepth == 16 && std::endian::little == std::endian::native) {
        png_set_swap(pngPtr);
    }

    optional<AttributeNode> exifAttributes;

    png_uint_32 exifDataSize = 0;
    png_bytep exifDataRaw = nullptr;
    png_get_eXIf_1(pngPtr, infoPtr, &exifDataSize, &exifDataRaw);

    EOrientation orientation = EOrientation::TopLeft;
    if (exifDataRaw) {
        std::vector<uint8_t> exifData(exifDataRaw, exifDataRaw + exifDataSize);

        tlog::debug() << fmt::format("Found EXIF data of size {} bytes", exifData.size());

        try {
            const auto exif = Exif(exifData);
            exifAttributes = exif.toAttributes();
            orientation = exif.getOrientation();
            tlog::debug() << fmt::format("EXIF image orientation: {}", (int)orientation);
        } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
    }

    const bool hasAlpha = numChannels > numColorChannels;

    png_bytep iccProfileData = nullptr;
    png_charp iccProfileName = nullptr;
    png_uint_32 iccProfileSize = 0;
    if (png_get_iCCP(pngPtr, infoPtr, &iccProfileName, nullptr, &iccProfileData, &iccProfileSize) == PNG_INFO_iCCP) {
        tlog::debug() << fmt::format("Found ICC color profile: {}", iccProfileName);
    }

    png_uint_32 numFrames = png_get_num_frames(pngPtr, infoPtr);
    const bool isAnimated = numFrames > 1;
    if (isAnimated) {
        tlog::debug() << fmt::format("Image is an animated PNG with {} frames", numFrames);
    }

    const auto numPixels = static_cast<size_t>(size.x()) * size.y();
    const auto numBytesPerSample = static_cast<size_t>(bitDepth / 8);
    const auto numBytesPerPixel = numBytesPerSample * numChannels;
    const auto numSamples = numPixels * numChannels;

    // Allocate enough memory for each frame. By making the data as big as the whole canvas, all frames should fit.
    vector<png_byte> pngData(numPixels * numBytesPerPixel);
    vector<float> frameData(numSamples);
    vector<float> iccTmpFloatData;
    if (iccProfileData) {
        // If we have an ICC profile, we need to convert the frame data to float first.
        iccTmpFloatData.resize(numSamples);
    }

    // Png wants to read into a 2D array of pointers to rows, so we need to create that as well
    vector<png_bytep> rowPointers(height);

    vector<ImageData> result;

    const float* prevCanvas = nullptr;

    auto readFrame = [&]() -> Task<ImageData> {
        ImageData resultData;
        if (exifAttributes) {
            resultData.attributes.emplace_back(exifAttributes.value());
        }

        // PNG images have a fixed point representation of up to 16 bits per channel in TF space. FP16 is perfectly adequate to represent
        // such values after conversion to linear space.
        resultData.channels = makeRgbaInterleavedChannels(numChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16);
        resultData.orientation = orientation;
        resultData.hasPremultipliedAlpha = false;

        enum class EDisposeOp : png_byte { None = 0, Background = 1, Previous = 2 };
        enum class EBlendOp : png_byte { Source = 0, Over = 1 };

        Vector2i frameSize;
        Vector2i frameOffset;

        png_uint_32 frameSizeX, frameSizeY, xOffset, yOffset;
        png_uint_16 delayNum, delayDen;
        EDisposeOp disposeOp;
        EBlendOp blendOp;

        if (png_get_next_frame_fcTL(
                pngPtr, infoPtr, &frameSizeX, &frameSizeY, &xOffset, &yOffset, &delayNum, &delayDen, (png_byte*)&disposeOp, (png_byte*)&blendOp
            )) {
            frameSize = {static_cast<int>(frameSizeX), static_cast<int>(frameSizeY)};
            frameOffset = {static_cast<int>(xOffset), static_cast<int>(yOffset)};

            tlog::debug() << fmt::format(
                "fcTL: size={}, offset={}, dispose_op={}, blend_op={}", frameSize, frameOffset, (uint8_t)disposeOp, (uint8_t)blendOp
            );
        } else {
            // If we don't have an fcTL chunk, we must be the static frame of the PNG (IDAT chunk), *not* be part of the animation (else
            // there would have been an fcTL chunk before the IDAT chunk), and we fill the entire canvas.
            frameSize = size;
            frameOffset = {0, 0};
            disposeOp = EDisposeOp::None;
            blendOp = EBlendOp::Source;
        }

        // If our frame fills the entire canvas and is configured to overwrite the canvas (as is the case for static frames / PNGs), we can
        // directly write onto the canvas and not worry about blending.
        const bool directlyOnCanvas = frameOffset == Vector2i{0, 0} && frameSize == size && blendOp == EBlendOp::Source;
        float* const dstData = directlyOnCanvas ? resultData.channels.front().floatData() : frameData.data();

        const size_t numFramePixels = (size_t)frameSize.x() * frameSize.y();
        const size_t numFrameSamples = numFramePixels * numChannels;
        if (numFrameSamples > frameData.size()) {
            tlog::warning() << fmt::format(
                "PNG frame data is larger than allocated buffer. Allocating {} bytes instead of {} bytes.",
                numFrameSamples * sizeof(float),
                frameData.size() * sizeof(float)
            );

            frameData.resize(numFrameSamples);
            if (iccProfileData) {
                iccTmpFloatData.resize(numFrameSamples);
            }
        }

        for (int y = 0; y < frameSize.y(); ++y) {
            rowPointers[y] = &pngData[y * frameSize.x() * numBytesPerPixel];
        }

        png_read_image(pngPtr, rowPointers.data());

        auto applyColorspace = [&]() -> Task<void> {
            if (double maxCLL, maxFALL; png_get_cLLI(pngPtr, infoPtr, &maxCLL, &maxFALL) == PNG_INFO_cLLI) {
                tlog::info() << fmt::format("cLLI: maxCLL={} maxFALL={}", maxCLL, maxFALL);

                resultData.hdrMetadata.maxCLL = static_cast<float>(maxCLL);
                resultData.hdrMetadata.maxFALL = static_cast<float>(maxFALL);
            }

            // According to https://www.w3.org/TR/png-3/#color-chunk-precendence, if a cICP chunk exists, use it to convert to sRGB. Else,
            // if an iCCP chunk exists, use its embedded ICC color profile to convert to linear sRGB. Otherwise, check the sRGB chunk for
            // whether the image is in sRGB/Rec709. If not, then check the gAMA and cHRM chunks for gamma and chromaticity values and use
            // them to convert to linear sRGB. And lastly (this isn't in the spec, but based on having seen a lot of PNGs), if none of these
            // chunks are present, fall back to sRGB.
            struct cICP {
                png_byte colourPrimaries;
                png_byte transferFunction;
                png_byte matrixCoefficients;
                png_byte videoFullRangeFlag;
            } cicp;
            if (png_get_cICP(
                    pngPtr, infoPtr, &cicp.colourPrimaries, &cicp.transferFunction, &cicp.matrixCoefficients, &cicp.videoFullRangeFlag
                ) == PNG_INFO_cICP) {

                const auto primaries = (ituth273::EColorPrimaries)cicp.colourPrimaries;
                auto transfer = (ituth273::ETransferCharacteristics)cicp.transferFunction;

                if (!ituth273::isTransferImplemented(transfer)) {
                    tlog::warning(
                    ) << fmt::format("Unsupported transfer '{}' in cICP chunk. Using sRGB instead.", ituth273::toString(transfer));
                    transfer = ituth273::ETransferCharacteristics::SRGB;
                }

                tlog::debug() << fmt::format(
                    "cICP: primaries={} transfer={} full_range={}",
                    ituth273::toString(primaries),
                    ituth273::toString(transfer),
                    cicp.videoFullRangeFlag == 1 ? "yes" : "no"
                );

                const LimitedRange range = cicp.videoFullRangeFlag != 0 ? LimitedRange::full() : limitedRangeForBitsPerSample(bitDepth);

                if (cicp.matrixCoefficients != 0) {
                    tlog::warning() << fmt::format(
                        "Unsupported matrix coefficients in cICP chunk: {}. PNG images only support RGB (=0). Ignoring.", cicp.matrixCoefficients
                    );
                }

                if (bitDepth == 16) {
                    co_await toFloat32<uint16_t, false>((uint16_t*)pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
                } else {
                    co_await toFloat32<uint8_t, false>(pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
                }

                co_await ThreadPool::global().parallelForAsync<size_t>(
                    0,
                    numPixels,
                    [&](size_t i) {
                        const float alpha = dstData[i * 4 + 3];
                        for (size_t c = 0; c < 3; ++c) {
                            const float val = (dstData[i * 4 + c] - range.offset) * range.scale;
                            dstData[i * 4 + c] = ituth273::invTransfer(transfer, val) * alpha;
                        }
                    },
                    priority
                );
                resultData.toRec709 = chromaToRec709Matrix(ituth273::chroma(primaries));

                resultData.hasPremultipliedAlpha = true;

                co_return;
            } else if (iccProfileData) {
                try {
                    if (bitDepth == 16) {
                        co_await toFloat32(
                            (uint16_t*)pngData.data(), numChannels, iccTmpFloatData.data(), numChannels, size, hasAlpha, priority
                        );
                    } else {
                        co_await toFloat32(pngData.data(), numChannels, iccTmpFloatData.data(), numChannels, size, hasAlpha, priority);
                    }

                    co_await toLinearSrgbPremul(
                        ColorProfile::fromIcc(iccProfileData, iccProfileSize),
                        size,
                        numColorChannels,
                        numChannels > numColorChannels ? EAlphaKind::Straight : EAlphaKind::None,
                        EPixelFormat::F32,
                        (uint8_t*)iccTmpFloatData.data(),
                        dstData,
                        4,
                        priority
                    );

                    resultData.hasPremultipliedAlpha = true;
                    co_return;
                } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
            }

            int srgbIntent = 0;
            const bool hasChunkSrgb = png_get_sRGB(pngPtr, infoPtr, &srgbIntent) == PNG_INFO_iCCP;

            double invGamma64 = 1.0 / 2.2;
            const bool hasChunkGama = png_get_gAMA(pngPtr, infoPtr, &invGamma64) == PNG_INFO_gAMA;
            const float gamma = 1.0f / (float)invGamma64;

            array<double, 8> ch = {0};
            const bool hasChunkChrm = png_get_cHRM(pngPtr, infoPtr, &ch[0], &ch[1], &ch[2], &ch[3], &ch[4], &ch[5], &ch[6], &ch[7]) ==
                PNG_INFO_cHRM;

            const bool useSrgb = hasChunkSrgb || (!hasChunkGama && !hasChunkChrm);
            if (useSrgb) {
                if (hasChunkSrgb) {
                    tlog::debug() << fmt::format("Using sRGB chunk w/ rendering intent {}", srgbIntent);
                } else {
                    tlog::debug() << "No cICP, iCCP, sRGB, gAMA, or cHRM chunks found. Using sRGB by default.";
                }

                if (bitDepth == 16) {
                    co_await toFloat32<uint16_t, true, true>((uint16_t*)pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
                } else {
                    co_await toFloat32<uint8_t, true, true>(pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
                }

                resultData.hasPremultipliedAlpha = true;
                co_return;
            }

            tlog::debug() << fmt::format("Using gamma={}", invGamma64);
            if (bitDepth == 16) {
                co_await toFloat32<uint16_t, false>((uint16_t*)pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
            } else {
                co_await toFloat32<uint8_t, false>(pngData.data(), numChannels, dstData, 4, size, hasAlpha, priority);
            }

            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                numPixels,
                [&](size_t i) {
                    const float alpha = dstData[i * 4 + 3];
                    for (int c = 0; c < 3; ++c) {
                        dstData[i * 4 + c] = pow(dstData[i * 4 + c], gamma) * alpha;
                    }
                },
                priority
            );

            resultData.hasPremultipliedAlpha = true;

            if (hasChunkChrm) {
                array<Vector2f, 4> chroma = {
                    {
                     {(float)ch[2], (float)ch[3]}, // red
                        {(float)ch[4], (float)ch[5]}, // green
                        {(float)ch[6], (float)ch[7]}, // blue
                        {(float)ch[0], (float)ch[1]}, // white
                    }
                };
                resultData.toRec709 = chromaToRec709Matrix(chroma);

                tlog::debug() << fmt::format("cHRM: primaries={}", chroma);
            }
        };

        co_await applyColorspace();

        if (!directlyOnCanvas) {
            tlog::debug() << "Blending frame onto previous canvas";

            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                [&](int y) {
                    Vector2i framePos;
                    framePos.y() = y - frameOffset.y();
                    for (int x = 0; x < size.x(); ++x) {
                        const size_t canvasPixelIdx = (size_t)y * size.x() + (size_t)x;
                        framePos.x() = x - frameOffset.x();

                        bool isInFrame = Box2i{frameSize}.contains(framePos);

                        for (int c = 0; c < numChannels; ++c) {
                            const size_t canvasSampleIdx = canvasPixelIdx * numChannels + c;
                            float val;

                            // The background (if no previous canvas is set) is defined as transparent black per the spec.
                            const float bg = prevCanvas ? prevCanvas[canvasSampleIdx] : 0.0f;
                            if (isInFrame) {
                                const size_t framePixelIdx = (size_t)framePos.y() * frameSize.x() + (size_t)framePos.x();
                                const size_t frameSampleIdx = framePixelIdx * numChannels + c;
                                const size_t frameAlphaIdx = framePixelIdx * numChannels + 3;

                                if (blendOp == EBlendOp::Source) {
                                    val = dstData[frameSampleIdx];
                                } else {
                                    val = dstData[frameSampleIdx] + bg * (1.0f - dstData[frameAlphaIdx]);
                                }
                            } else {
                                val = bg;
                            }

                            resultData.channels.front().floatData()[canvasSampleIdx] = val;
                        }
                    }
                },
                priority
            );
        }

        // Depending on the dispose operation, the next frame will be blended either onto the current frame (none), onto no frame at all
        // (background), or the previous frame (previous).
        switch (disposeOp) {
            case EDisposeOp::None: prevCanvas = resultData.channels.front().floatData(); break;
            case EDisposeOp::Background: prevCanvas = nullptr; break;
            case EDisposeOp::Previous: break; // Previous frame is already set as the previous canvas
            default: throw ImageLoadError{fmt::format("Unsupported PNG dispose operation: {}", (uint8_t)disposeOp)};
        }

        co_return resultData;
    };

    for (png_uint_32 i = 0; i < numFrames; ++i) {
        if (isAnimated) {
            png_read_frame_head(pngPtr, infoPtr);
            tlog::debug() << fmt::format("Reading frame {}/{}", i + 1, numFrames);
        }

        result.emplace_back(co_await readFrame());
        if (isAnimated) {
            result.back().partName = fmt::format("frames.{}", i);
        }
    }

    co_return result;
}

} // namespace tev
