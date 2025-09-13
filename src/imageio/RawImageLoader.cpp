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
#include <tev/imageio/RawImageLoader.h>

#define LIBRAW_NO_WINSOCK2
#include <libraw/libraw.h>

using namespace nanogui;
using namespace std;

namespace tev {

class LibRawDataStream : public LibRaw_abstract_datastream {
public:
    LibRawDataStream(istream& stream, const fs::path& path) : stream{stream} {
        auto pathStr = toString(path);
        strncpy(mPath, pathStr.c_str(), sizeof(mPath) - 1);

#ifdef LIBRAW_WIN32_UNICODEPATHS
        auto wPathStr = path.wstring();
        wcsncpy(mWPath, wPathStr.c_str(), sizeof(mWPath) / sizeof(mWPath[0]) - 1);
#endif
    }

    ~LibRawDataStream() override = default;

    int valid() override { return stream.good(); }

    int read(void* ptr, size_t size, size_t nmemb) override {
        stream.read((char*)ptr, size * nmemb);
        return stream.gcount() / size;
    }

    int seek(INT64 o, int whence) override {
        ios_base::seekdir dir;
        switch (whence) {
            case SEEK_SET: dir = ios_base::beg; break;
            case SEEK_CUR: dir = ios_base::cur; break;
            case SEEK_END: dir = ios_base::end; break;
            default: return -1;
        }
        stream.clear(); // Clear any eof flags
        stream.seekg(o, dir);
        return stream.good() ? 0 : -1;
    }

    INT64 tell() override { return stream.tellg(); }

    INT64 size() override {
        auto currentPos = stream.tellg();
        stream.seekg(0, ios_base::end);
        auto size = stream.tellg();
        stream.seekg(currentPos, ios_base::beg);
        return size;
    }

    int get_char() override { return stream.get(); }

    char* gets(char* str, int sz) override {
        stream.getline(str, sz);
        return stream.good() ? str : nullptr;
    }

    int scanf_one(const char* fmt, void* val) override {
        // Not implemented
        return -1;
    }

    int eof() override { return stream.eof(); }

    int jpeg_src(void* /*jpegdata*/) override {
        // Not implemented
        return -1;
    }

    const char* fname() override { return mPath; };
#ifdef LIBRAW_WIN32_UNICODEPATHS
    const wchar_t* wfname() override { return mWPath; };
#endif

private:
    istream& stream;

    char mPath[1024] = {};
#ifdef LIBRAW_WIN32_UNICODEPATHS
    wchar_t mWPath[1024] = {};
#endif
};

Vector2i flip_index(Vector2i idx, const Vector2i& size, int flip) {
    if (flip & 4) {
        swap(idx.x(), idx.y());
    }

    if (flip & 1) {
        idx.y() = size.y() - 1 - idx.y();
    }

    if (flip & 2) {
        idx.x() = size.x() - 1 - idx.x();
    }

    return idx;
}

Box2i cropToBox(const libraw_raw_inset_crop_t& crop) {
    return {
        {crop.cleft,               crop.ctop               },
        {crop.cleft + crop.cwidth, crop.ctop + crop.cheight},
    };
}

Box2i maskToBox(const int mask[4]) {
    return {
        {mask[0], mask[1]},
        {mask[2], mask[3]},
    };
}

Task<vector<ImageData>> RawImageLoader::load(istream& iStream, const fs::path& path, string_view, int priority, bool) const {
    LibRaw iProcessor;

    iProcessor.imgdata.params.use_camera_matrix = true;
    iProcessor.imgdata.params.use_camera_wb = true;

    auto librawStream = LibRawDataStream{iStream, path};
    if (int error = iProcessor.open_datastream(&librawStream); error != LIBRAW_SUCCESS) {
        throw FormatNotSupported{fmt::format("Could not open raw image: {}", libraw_strerror(error))};
    }

    if (int error = iProcessor.unpack(); error != LIBRAW_SUCCESS) {
        throw ImageLoadError{fmt::format("Could not unpack raw image: {}", libraw_strerror(error))};
    }

    if (int error = iProcessor.dcraw_process(); error != LIBRAW_SUCCESS) {
        throw ImageLoadError{fmt::format("Could not process raw image: {}", libraw_strerror(error))};
    }

    const Vector2i size = {iProcessor.imgdata.sizes.iwidth, iProcessor.imgdata.sizes.iheight};
    const int flip = iProcessor.imgdata.sizes.flip;
    const Vector2i orientedSize = (flip & 4) ? Vector2i{size.y(), size.x()} : size;

    const float fmax = iProcessor.imgdata.color.fmaximum;
    const float fnorm = iProcessor.imgdata.color.fnorm;

    Box2i displayWindow = {
        {0, 0},
        size
    };
    for (int i = 0; i < 8; i++) {
        Box2i box = maskToBox(iProcessor.imgdata.sizes.mask[i]);
        tlog::debug() << fmt::format("mask[{}] = [{}, {}]", i, box.min, box.max);

        if (!box.isValid() || box.area() == 0) {
            continue;
        }

        displayWindow = displayWindow.intersect(box);
    }

    for (int i = 0; i < 2; i++) {
        Box2i box = cropToBox(iProcessor.imgdata.sizes.raw_inset_crops[i]);
        tlog::debug() << fmt::format("raw_inset_crops[{}] = [{}, {}]", i, box.min, box.max);

        if (!box.isValid() || box.area() == 0) {
            continue;
        }

        displayWindow = displayWindow.intersect(box);
    }

    if (flip & 4) {
        displayWindow = {
            {displayWindow.min.y(), displayWindow.min.x()},
            {displayWindow.max.y(), displayWindow.max.x()}
        };
    }

    const Vector2i margin = {iProcessor.imgdata.sizes.left_margin, iProcessor.imgdata.sizes.top_margin};
    tlog::debug() << fmt::format(
        "raw image: size={} flip={} crop=[{}, {}] margin={} fmax={} fnorm={}",
        orientedSize,
        flip,
        displayWindow.min,
        displayWindow.max,
        margin,
        fmax,
        fnorm
    );

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    const int numChannels = 3;
    resultData.channels = makeRgbaInterleavedChannels(numChannels, numChannels == 4, orientedSize, EPixelFormat::F32, EPixelFormat::F16);
    resultData.hasPremultipliedAlpha = false;
    // resultData.displayWindow = displayWindow; // This seems to be wrong

    const auto* imgData = iProcessor.imgdata.image;
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); x++) {
                const size_t i = (size_t)y * size.x() + x;
                const auto fi = flip_index({x, y}, orientedSize, flip);
                const size_t j = (size_t)fi.y() * orientedSize.x() + fi.x();

                for (int c = 0; c < numChannels; c++) {
                    resultData.channels[c].setAt(j, imgData[i][c] / 65535.0f);
                }
            }
        },
        priority
    );

    co_return result;
}

} // namespace tev
