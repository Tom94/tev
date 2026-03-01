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

#include <tev/imageio/PngImageSaver.h>

#include <libpng/png.h>
#include <zlib.h>

#include <ostream>
#include <span>

using namespace nanogui;
using namespace std;

namespace tev {

Task<void> PngImageSaver::save(ostream& oStream, const fs::path&, span<const uint8_t> data, const Vector2i& imageSize, int nChannels) const {
    int colorType;
    switch (nChannels) {
        case 1: colorType = PNG_COLOR_TYPE_GRAY; break;
        case 2: colorType = PNG_COLOR_TYPE_GRAY_ALPHA; break;
        case 3: colorType = PNG_COLOR_TYPE_RGB; break;
        case 4: colorType = PNG_COLOR_TYPE_RGB_ALPHA; break;
        default: throw ImageSaveError{fmt::format("Invalid number of channels {}.", nChannels)};
    }

    png_infop info = nullptr;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    const ScopeGuard pngGuard{[&]() { png_destroy_write_struct(&png, &info); }};
    if (!png) {
        throw ImageSaveError{"Failed to create PNG write struct."};
    }

    png_set_error_fn(
        png,
        nullptr,
        [](png_structp, png_const_charp error_msg) { throw ImageLoadError{fmt::format("PNG error: {}", error_msg)}; },
        [](png_structp, png_const_charp warning_msg) { tlog::warning() << fmt::format("PNG warning: {}", warning_msg); }
    );

    info = png_create_info_struct(png);
    if (!info) {
        throw ImageSaveError{"Failed to create PNG info struct."};
    }

    png_set_compression_level(png, 1);
    png_set_compression_strategy(png, Z_RLE);
    png_set_filter(png, 0, PNG_FAST_FILTERS);

    static const auto pngWriteCallback = [](png_structp png, png_bytep buf, png_size_t size) {
        auto* os = static_cast<ostream*>(png_get_io_ptr(png));
        os->write(reinterpret_cast<char*>(buf), size);
    };

    static const auto pngFlushCallback = [](png_structp png) {
        auto* os = static_cast<ostream*>(png_get_io_ptr(png));
        os->flush();
    };

    png_set_write_fn(png, &oStream, pngWriteCallback, pngFlushCallback);

    png_set_IHDR(
        png, info, imageSize.x(), imageSize.y(), 8, colorType, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT
    );

    png_write_info(png, info);

    const auto stride = (size_t)imageSize.x() * nChannels;
    for (int y = 0; y < imageSize.y(); ++y) {
        png_write_row(png, data.data() + y * stride);
    }

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);

    co_return;
}

} // namespace tev
