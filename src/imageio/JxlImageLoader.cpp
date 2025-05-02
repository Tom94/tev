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

#include <lcms2.h>
#include <lcms2_fast_float.h>

#include <istream>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {
// Helper to identify JPEG XL files by signature
bool isJxlImage(istream& iStream) {
    const size_t signatureSize = 16;
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

JxlImageLoader::JxlImageLoader() {
    cmsSetLogErrorHandler([](cmsContext, cmsUInt32Number errorCode, const char* message) {
        tlog::error() << fmt::format("lcms error #{}: {}", errorCode, message);
    });

    cmsPlugin(cmsFastFloatExtensions());
}

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

    auto runner = JxlThreadParallelRunnerMake(nullptr, thread::hardware_concurrency());
    if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw runtime_error{"Failed to set parallel runner for JPEG XL decoder."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE)) {
        throw runtime_error{"Failed to subscribe to JPEG XL decoder events."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetInput(decoder.get(), fileData.data(), fileData.size())) {
        throw runtime_error{"Failed to set input for JPEG XL decoder."};
    }

    if (JXL_DEC_SUCCESS != JxlDecoderSetCms(decoder.get(), *JxlGetDefaultCms())) {
        throw runtime_error{"Failed to set CMS for JPEG XL decoder."};
    }

    JxlBasicInfo info;
    JxlPixelFormat format;
    JxlColorEncoding colorEncoding;
    vector<float> pixels;
    bool hasAlpha = false;
    bool hasAlphaPremultiplied = false;

    // Create image data
    vector<ImageData> result;
    vector<uint8_t> iccProfile;

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
            hasAlphaPremultiplied = info.alpha_premultiplied > 0;
            format = {info.num_color_channels + info.num_extra_channels, JXL_TYPE_FLOAT, JXL_LITTLE_ENDIAN, 0};
            status = JxlDecoderProcessInput(decoder.get());
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            size_t size = 0;
            if (JxlDecoderGetICCProfileSize(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &size) != JXL_DEC_SUCCESS) {
                throw runtime_error{"Failed to get ICC profile size from JPEG XL image."};
            }

            iccProfile.resize(size);
            if (JxlDecoderGetColorAsICCProfile(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, iccProfile.data(), size) !=
                JXL_DEC_SUCCESS) {
                throw runtime_error{"Failed to get ICC profile from JPEG XL image."};
            }

            if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JxlColorProfileTarget::JXL_COLOR_PROFILE_TARGET_DATA, &colorEncoding) !=
                JXL_DEC_SUCCESS) {
                tlog::warning() << "Failed to get color encoding from JPEG XL image. Relying purely on ICC profile.";
            } else {
                if (colorEncoding.color_space == JxlColorSpace::JXL_COLOR_SPACE_XYB) {
                    tlog::warning() << "JPEG XL image has XYB color space. This might be broken.";
                    JxlColorEncodingSetToLinearSRGB(&colorEncoding, false /* XYB is never grayscale */);

                    // Clear the ICC profile and instead rely on the jxl decoder to give us linear sRGB
                    iccProfile.clear();
                }

                // Encourage the decoder to use the specified color profile, even if it is the one we just read
                if (JxlDecoderSetPreferredColorProfile(decoder.get(), &colorEncoding) != JXL_DEC_SUCCESS) {
                    throw runtime_error{"Failed to set preferred color profile for JPEG XL image."};
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
            if (status == JXL_DEC_ERROR) {
                std::cout << "Failed to process input for JPEG XL image: " << std::endl;
            }

            // Check if we got any pixels
            if (pixels.empty()) {
                throw runtime_error{"Failed to decode JPEG XL image data."};
            }

            cmsHTRANSFORM transform = nullptr;
            ScopeGuard transformGuard{[transform] {
                if (transform) {
                    cmsDeleteTransform(transform);
                }
            }};

            if (!iccProfile.empty()) {
                tlog::debug() << "Found ICC color profile. Attempting to apply...";

                // Create ICC profile from the raw data
                cmsHPROFILE srcProfile = cmsOpenProfileFromMem(iccProfile.data(), (cmsUInt32Number)iccProfile.size());
                if (!srcProfile) {
                    tlog::warning() << "Failed to create ICC profile from raw data.";
                }

                ScopeGuard srcProfileGuard{[srcProfile] { cmsCloseProfile(srcProfile); }};

                cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
                cmsCIExyYTRIPLE Rec709Primaries = {
                    {0.6400, 0.3300, 1.0},
                    {0.3000, 0.6000, 1.0},
                    {0.1500, 0.0600, 1.0}
                };

                cmsToneCurve* linearCurve[3];
                linearCurve[0] = linearCurve[1] = linearCurve[2] = cmsBuildGamma(0, 1.0f);

                cmsHPROFILE rec709Profile = cmsCreateRGBProfile(&D65, &Rec709Primaries, linearCurve);

                if (!rec709Profile) {
                    tlog::warning() << "Failed to create Rec.709 color profile.";
                }

                ScopeGuard rec709ProfileGuard{[rec709Profile] { cmsCloseProfile(rec709Profile); }};

                // Create transform from source profile to Rec.709
                // From the JxlPixelFormat docs:
                //   Amount of channels available in a pixel buffer.
                //     1: single-channel data, e.g. grayscale or a single extra channel
                //     2: single-channel + alpha
                //     3: trichromatic, e.g. RGB
                //     4: trichromatic + alpha
                //
                cmsUInt32Number type = 0;
                switch (format.num_channels) {
                    case 1: type = TYPE_GRAY_FLT; break;
                    case 2: type = TYPE_GRAYA_FLT; break;
                    case 3: type = TYPE_RGB_FLT; break;
                    case 4: type = TYPE_RGBA_FLT; break;
                    default:
                        // TODO: support >4 channels gracefully by appending extra channels without any color transform
                        tlog::warning() << "Unsupported number of channels in JPEG XL image: " << format.num_channels;
                        break;
                }

                if (type != 0) {
                    transform = cmsCreateTransform(srcProfile, type, rec709Profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);
                    if (!transform) {
                        tlog::warning() << "Failed to create color transform from ICC profile to Rec.709.";
                    }
                }
            }

            if (!transform) {
                tlog::warning() << "Loading JPEG XL image without color transform. This may result in incorrect colors.";
            }

            result.emplace_back();
            ImageData& data = result.back();

            // Set dimensions
            Vector2i size{(int)info.xsize, (int)info.ysize};

            // Create channels
            int numChannels = format.num_channels;
            data.channels = makeNChannels(numChannels, size);
            int alphaChannel = info.num_color_channels;

            // lcms can't deal with alpha premultiplication, so we operate on straight colors
            data.hasPremultipliedAlpha = false;

            const size_t n_samples_per_row = size.x() * numChannels;
            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                size.y(),
                [&](size_t y) {
                    size_t src_offset = y * n_samples_per_row;

                    if (transform) {
                        // This is horrible: apparently jxl's alpha premultiplication is not in linear space, so we need to unmultiply first.
                        if (hasAlphaPremultiplied) {
                            for (int x = 0; x < size.x(); ++x) {
                                const size_t baseIdx = src_offset + x * numChannels;
                                const float alpha = pixels[baseIdx + alphaChannel];
                                const float factor = alpha == 0.0f ? 0.0f : 1.0f / alpha;
                                for (int c = 0; c < numChannels - 1; ++c) {
                                    pixels[baseIdx + c] *= factor;
                                }
                            }
                        }

                        // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads.
                        // So: call cmsDoTransform for each row in parallel.
                        // NOTE: This code depends on makeNChannels creating RGBA interleaved buffers!
                        size_t dst_offset = y * (size_t)size.x() * 4;
                        cmsDoTransform(transform, &pixels[src_offset], &data.channels[0].data()[dst_offset], size.x());

                        if (hasAlpha) {
                            for (int x = 0; x < size.x(); ++x) {
                                size_t baseIdx = src_offset + x * numChannels;
                                data.channels[alphaChannel].at({x, (int)y}) = pixels[baseIdx + alphaChannel];
                            }
                        }
                    } else {
                        for (int x = 0; x < size.x(); ++x) {
                            size_t baseIdx = src_offset + x * numChannels;
                            for (int c = 0; c < numChannels; ++c) {
                                data.channels[c].at({x, (int)y}) = pixels[baseIdx + c];
                            }
                        }
                    }
                },
                priority
            );

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
