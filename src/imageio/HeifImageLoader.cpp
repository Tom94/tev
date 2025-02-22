// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ThreadPool.h>
#include <tev/imageio/HeifImageLoader.h>

#include <libheif/heif.h>

using namespace nanogui;
using namespace std;

namespace tev {

bool HeifImageLoader::canLoadFile(istream& iStream) const {
    // libheif's spec says it needs the first 12 bytes to determine whether the image can be read.
    uint8_t header[12];
    iStream.read((char*)header, 12);
    bool failed = !iStream || iStream.gcount() != 12;

    iStream.clear();
    iStream.seekg(0);
    if (failed) {
        return false;
    }

    return heif_check_filetype(header, 12) == heif_filetype_yes_supported;
}

Task<vector<ImageData>> HeifImageLoader::load(istream& iStream, const fs::path&, const string& channelSelector, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    struct ReaderContext {
        istream& stream;
        int64_t size;
    } readerContext = {iStream, fileSize};

    static const heif_reader reader = {
        .reader_api_version = 1,
        .get_position = [](void* context) { return (int64_t)reinterpret_cast<ReaderContext*>(context)->stream.tellg(); },
        .read =
            [](void* data, size_t size, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.read((char*)data, size);
                return stream.good() ? 0 : -1;
            },
        .seek =
            [](int64_t pos, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.seekg(pos);
                return stream.good() ? 0 : -1;
            },
        .wait_for_file_size =
            [](int64_t target_size, void* context) {
                return reinterpret_cast<ReaderContext*>(context)->size < target_size ? heif_reader_grow_status_size_beyond_eof :
                                                                                       heif_reader_grow_status_size_reached;
            },
        // Not used by API version 1
        .request_range = {},
        .preload_range_hint = {},
        .release_file_range = {},
        .release_error_msg = {},
    };

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        throw invalid_argument{"Failed to allocate libheif context."};
    }

    ScopeGuard contextGuard{[ctx] { heif_context_free(ctx); }};

    if (auto error = heif_context_read_from_reader(ctx, &reader, &readerContext, nullptr); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to read image: {}", error.message)};
    }

    // get a handle to the primary image
    heif_image_handle* handle;
    if (auto error = heif_context_get_primary_image_handle(ctx, &handle); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to get primary image handle: {}", error.message)};
    }

    ScopeGuard handleGuard{[handle] { heif_image_handle_release(handle); }};

    int numChannels = heif_image_handle_has_alpha_channel(handle) ? 4 : 3;
    bool hasPremultipliedAlpha = numChannels == 4 && heif_image_handle_is_premultiplied_alpha(handle);

    const bool is_little_endian = std::endian::native == std::endian::little;
    auto format = numChannels == 4 ? (is_little_endian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE) :
                                     (is_little_endian ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE);

    Vector2i size = {heif_image_handle_get_width(handle), heif_image_handle_get_height(handle)};

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    heif_image* img;
    if (auto error = heif_decode_image(handle, &img, heif_colorspace_RGB, format, nullptr); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to decode image: {}", error.message)};
    }

    ScopeGuard imgGuard{[img] { heif_image_release(img); }};

    const int bitsPerPixel = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
    const float channelScale = 1.0f / float((1 << bitsPerPixel) - 1);

    int bytesPerLine;
    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &bytesPerLine);
    if (!data) {
        throw invalid_argument{"Faild to get image data."};
    }

    resultData.channels = makeNChannels(numChannels, size);

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                size_t i = y * (size_t)size.x() + x;
                auto typedData = reinterpret_cast<const unsigned short*>(data + y * bytesPerLine);
                int baseIdx = x * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    if (c == 3) {
                        resultData.channels[c].at(i) = typedData[baseIdx + c] * channelScale;
                    } else {
                        resultData.channels[c].at(i) = toLinear(typedData[baseIdx + c] * channelScale);
                    }
                }
            }
        },
        priority
    );

    resultData.hasPremultipliedAlpha = hasPremultipliedAlpha;

    co_return result;
}

} // namespace tev
