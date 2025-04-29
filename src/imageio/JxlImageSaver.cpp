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

#include <tev/imageio/JxlImageSaver.h>

#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>

#include <ostream>
#include <string>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

void JxlImageSaver::save(ostream& oStream, const fs::path& path, const vector<float>& data, const Vector2i& imageSize, int nChannels) const {
    if (nChannels <= 0 || nChannels > 4098) {
        throw invalid_argument{fmt::format("Invalid number of channels {}.", nChannels)};
    }

    // Create JXL encoder
    auto encoder = JxlEncoderMake(nullptr);
    if (!encoder) {
        throw runtime_error{"Failed to create JPEG XL encoder."};
    }

    // Set up parallel runner
    auto runner = JxlThreadParallelRunnerMake(nullptr, 16);//ThreadPool::global().threadCount());
    if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get())) {
        throw runtime_error{"Failed to set parallel runner for JPEG XL encoder."};
    }

    // Configure encoder options for lossless HDR output
    JxlEncoderFrameSettings* options = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    if (!options) {
        throw runtime_error{"Failed to create JPEG XL encoder options."};
    }

    // Set the effort level (0-9, 9 is the highest quality)
    if (JXL_ENC_SUCCESS != JxlEncoderFrameSettingsSetOption(options, JXL_ENC_FRAME_SETTING_EFFORT, 7)){
        throw runtime_error{"Failed to set effort level for JPEG XL encoder."};
    }

    // Set encoder to lossless mode
    if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(options, 1)) {
        throw runtime_error{"Failed to set lossless mode for JPEG XL encoder."};
    }

    // Create basic info
    JxlBasicInfo basic_info;
    JxlEncoderInitBasicInfo(&basic_info);
    basic_info.xsize = imageSize.x();
    basic_info.ysize = imageSize.y();
    basic_info.bits_per_sample = 32;
    basic_info.exponent_bits_per_sample = 8; // For floating point
    basic_info.uses_original_profile = JXL_TRUE;
    
    // Set alpha information if needed
    if (nChannels == 2 || nChannels == 4) {
        basic_info.alpha_bits = 32;
        basic_info.alpha_exponent_bits = 8;
        basic_info.alpha_premultiplied = JXL_TRUE;
    }

    if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(encoder.get(), &basic_info)) {
        throw runtime_error{"Failed to set basic info for JPEG XL encoder."};
    }

    // Set color encoding to linear sRGB
    JxlColorEncoding color_encoding;
    JxlColorEncodingSetToLinearSRGB(&color_encoding, (nChannels >= 3) ? JXL_TRUE : JXL_FALSE);
    
    if (JXL_ENC_SUCCESS != JxlEncoderSetColorEncoding(encoder.get(), &color_encoding)) {
        throw runtime_error{"Failed to set color encoding for JPEG XL encoder."};
    }

    // Set up the pixel format for input
    JxlPixelFormat pixel_format = {
        static_cast<uint32_t>(nChannels), // num_channels
        JXL_TYPE_FLOAT,                   // data_type
        JXL_LITTLE_ENDIAN,                // endianness
        0                                 // align
    };

    // Add the frame to be encoded
    if (JXL_ENC_SUCCESS != JxlEncoderAddImageFrame(options, &pixel_format, data.data(), data.size() * sizeof(float))) {
        throw runtime_error{"Failed to add image frame to JPEG XL encoder."};
    }

    // Close the encoder to finalize codestream
    JxlEncoderCloseInput(encoder.get());

    // Process the encoded image
    vector<uint8_t> compressed;
    compressed.resize(64 * 1024); // Start with a reasonably sized buffer
    size_t available_out = compressed.size();
    uint8_t* next_out = compressed.data();
    JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;

    while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
        process_result = JxlEncoderProcessOutput(encoder.get(), &next_out, &available_out);
        
        if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
            // We need to enlarge the buffer
            size_t offset = next_out - compressed.data();
            compressed.resize(compressed.size() * 2);
            next_out = compressed.data() + offset;
            available_out = compressed.size() - offset;
        }
    }

    if (process_result != JXL_ENC_SUCCESS) {
        throw runtime_error{"Failed to process output for JPEG XL encoder."};
    }

    // Trim the output buffer to the actual size
    compressed.resize(compressed.size() - available_out);

    // Write the compressed data to the output stream
    oStream.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    if (!oStream) {
        throw runtime_error{fmt::format("Failed to write JPEG XL data to {}.", toString(path))};
    }
}

} // namespace tev
