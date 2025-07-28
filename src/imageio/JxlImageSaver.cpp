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

#include <tev/imageio/JxlImageSaver.h>

#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <ostream>
#include <span>
#include <thread>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

void JxlImageSaver::save(ostream& oStream, const fs::path& path, span<const float> data, const Vector2i& imageSize, int nChannels) const {
    if (nChannels <= 0 || nChannels > 4098) {
        throw invalid_argument{fmt::format("Invalid number of channels {}.", nChannels)};
    }

    auto encoder = JxlEncoderMake(nullptr);
    if (!encoder) {
        throw ImageSaveError{"Failed to create encoder."};
    }

    auto runner = JxlThreadParallelRunnerMake(nullptr, thread::hardware_concurrency());
    if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw ImageSaveError{"Failed to set parallel runner."};
    }

    // Configure encoder options for lossless HDR output
    JxlEncoderFrameSettings* options = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    if (!options) {
        throw ImageSaveError{"Failed to create encoder options."};
    }

    // Set the effort level (0-9, 9 is the highest quality)
    if (JXL_ENC_SUCCESS != JxlEncoderFrameSettingsSetOption(options, JXL_ENC_FRAME_SETTING_EFFORT, 7)) {
        throw ImageSaveError{"Failed to set effort level."};
    }

    if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(options, 1)) {
        throw ImageSaveError{"Failed to set lossless mode."};
    }

    JxlBasicInfo basicInfo;
    JxlEncoderInitBasicInfo(&basicInfo);
    basicInfo.xsize = imageSize.x();
    basicInfo.ysize = imageSize.y();
    basicInfo.bits_per_sample = 32;
    basicInfo.exponent_bits_per_sample = 8; // ieee single precision floating point
    basicInfo.uses_original_profile = JXL_TRUE;

    bool hasAlpha = nChannels == 2 || nChannels == 4;
    if (hasAlpha) {
        basicInfo.alpha_bits = 32;
        basicInfo.alpha_exponent_bits = 8;
        basicInfo.alpha_premultiplied = JXL_TRUE;
    }

    basicInfo.num_color_channels = nChannels - (hasAlpha ? 1 : 0);
    basicInfo.num_extra_channels = hasAlpha ? 1 : 0;

    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(encoder.get(), &basicInfo)) {
        throw ImageSaveError{"Failed to set basic info."};
    }

    // Since JXL treats alpha channels as extra channels, a bit of redundant information needs to be attached to it here.
    if (hasAlpha) {
        JxlExtraChannelInfo alphaChannelInfo;
        JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &alphaChannelInfo);
        alphaChannelInfo.bits_per_sample = basicInfo.alpha_bits;
        alphaChannelInfo.exponent_bits_per_sample = basicInfo.alpha_exponent_bits;
        alphaChannelInfo.alpha_premultiplied = basicInfo.alpha_premultiplied;

        if (JXL_ENC_SUCCESS != JxlEncoderSetExtraChannelInfo(encoder.get(), 0, &alphaChannelInfo)) {
            throw ImageSaveError{
                fmt::format("Failed to set extra channel info for the alpha channel: {}.", (size_t)JxlEncoderGetError(encoder.get()))
            };
        }
    }

    JxlColorEncoding colorEncoding;
    JxlColorEncodingSetToLinearSRGB(&colorEncoding, nChannels == 1 ? JXL_TRUE : JXL_FALSE); // Assume 1 channel is grayscale

    if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(encoder.get(), &colorEncoding)) {
        throw ImageSaveError{"Failed to set color encoding."};
    }

    JxlPixelFormat pixelFormat = {
        static_cast<uint32_t>(nChannels), // num_channels
        JXL_TYPE_FLOAT,                   // data_type
        JXL_LITTLE_ENDIAN,                // endianness
        0                                 // align
    };

    // Add the frame to be encoded
    if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(options, &pixelFormat, data.data(), data.size() * sizeof(float))) {
        throw ImageSaveError{"Failed to add image frame to encoder."};
    }

    JxlEncoderCloseInput(encoder.get());

    // Encode the image and write it to file in 1mb chunks
    vector<uint8_t> compressed(1024 * 1024);
    while (true) {
        uint8_t* nextOut = compressed.data();
        size_t availableOut = compressed.size();
        JxlEncoderStatus processResult = JxlEncoderProcessOutput(encoder.get(), &nextOut, &availableOut);
        if (processResult == JXL_ENC_ERROR) {
            throw ImageSaveError{fmt::format("Failed to process output: {}.", (size_t)JxlEncoderGetError(encoder.get()))};
        }

        oStream.write(reinterpret_cast<const char*>(compressed.data()), compressed.size() - availableOut);
        if (!oStream) {
            throw ImageSaveError{fmt::format("Failed to write data to {}.", toString(path))};
        }

        if (processResult == JXL_ENC_SUCCESS) {
            break; // Encoding is done
        }
    }
}

} // namespace tev
