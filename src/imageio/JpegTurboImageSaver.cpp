/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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

#include <tev/imageio/JpegTurboImageSaver.h>

#include <jpeglib.h>

#include <ostream>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

Task<void> JpegTurboImageSaver::save(ostream& oStream, const fs::path&, span<const uint8_t> data, const Vector2i& imageSize, int nChannels) const {
    J_COLOR_SPACE colorSpace;
    switch (nChannels) {
        case 1: colorSpace = JCS_GRAYSCALE; break;
        case 3: colorSpace = JCS_RGB; break;
        case 4: colorSpace = JCS_EXT_RGBX; break;
        default: throw ImageSaveError{fmt::format("JPEG does not support {} channels.", nChannels)};
    }

    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr cinfo) {
        char buf[JMSG_LENGTH_MAX];
        cinfo->err->format_message(cinfo, buf);
        throw ImageLoadError{fmt::format("libjpeg error: {}", buf)};
    };
    jerr.output_message = [](j_common_ptr cinfo) {
        char buf[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message)(cinfo, buf);
        tlog::warning() << fmt::format("libjpeg warning: {}", buf);
    };

    jpeg_create_compress(&cinfo);
    const ScopeGuard cinfoGuard{[&]() { jpeg_destroy_compress(&cinfo); }};

    // Use libjpeg's memory destination; we'll copy to the ostream afterward.
    unsigned char* outBuffer = nullptr;
    unsigned long outSize = 0;
    jpeg_mem_dest(&cinfo, &outBuffer, &outSize);
    const ScopeGuard outBufferGuard{[&]() { free(outBuffer); }};

    cinfo.image_width = imageSize.x();
    cinfo.image_height = imageSize.y();
    cinfo.input_components = nChannels;
    cinfo.in_color_space = colorSpace;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    const auto stride = (size_t)imageSize.x() * nChannels;
    HeapArray<JSAMPROW> rowPointers(imageSize.y());
    for (int y = 0; y < imageSize.y(); ++y) {
        rowPointers[y] = const_cast<JSAMPROW>(data.data() + y * stride);
    }

    while (cinfo.next_scanline < cinfo.image_height) {
        jpeg_write_scanlines(&cinfo, &rowPointers[cinfo.next_scanline], cinfo.image_height - cinfo.next_scanline);
    }

    jpeg_finish_compress(&cinfo);

    oStream.write(reinterpret_cast<char*>(outBuffer), outSize);

    co_return;
}

} // namespace tev
