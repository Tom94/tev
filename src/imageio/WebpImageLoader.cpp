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

#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/WebpImageLoader.h>

#include <webp/decode.h>
#include <webp/demux.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> WebpImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool) const {
    char magic[16] = {0};
    iStream.read(magic, sizeof(magic));
    if (!iStream || strncmp(magic, "RIFF", 4) != 0 || strncmp(magic + 8, "WEBP", 4) != 0) {
        throw FormatNotSupported{"File is not a webp image."};
    }

    iStream.seekg(0, ios::end);
    size_t fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    vector<uint8_t> buffer(fileSize);
    iStream.read((char*)buffer.data(), fileSize);

    WebPData data = {buffer.data(), buffer.size()};
    WebPDemuxer* demux = WebPDemux(&data);
    if (!demux) {
        throw ImageLoadError{"Failed to demux webp image."};
    }

    ScopeGuard demuxGuard{[demux] { WebPDemuxDelete(demux); }};

    uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);

    WebPChunkIterator chunkIter;

    unique_ptr<ColorProfile> iccProfile;
    if (flags & ICCP_FLAG) {
        if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunkIter)) {
            ScopeGuard chunkGuard{[&chunkIter] { WebPDemuxReleaseChunkIterator(&chunkIter); }};
            try {
                tlog::debug() << "Found ICC color profile. Attempting to apply...";
                iccProfile = make_unique<ColorProfile>(ColorProfile::fromIcc(chunkIter.chunk.bytes, chunkIter.chunk.size));
            } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to create ICC color profile: {}", e.what()); }
        } else {
            tlog::warning() << "Failed to get ICCP chunk from webp image, despite flag being set.";
        }
    }

    optional<AttributeNode> exifAttributes;
    if (flags & EXIF_FLAG) {
        if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunkIter)) {
            ScopeGuard chunkGuard{[&chunkIter] { WebPDemuxReleaseChunkIterator(&chunkIter); }};

            try {
                vector<uint8_t> exifData(chunkIter.chunk.bytes, chunkIter.chunk.bytes + chunkIter.chunk.size);

                auto exif = Exif(exifData);
                exifAttributes = exif.toAttributes();
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
        } else {
            tlog::warning() << "Failed to get EXIF chunk from webp image, despite flag being set.";
        }
    }

    vector<ImageData> result;

    WebPIterator iter;
    size_t frameIdx = 0;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            const int numChannels = 4;
            const int numColorChannels = 3;

            Vector2i size;
            uint8_t* data = WebPDecodeRGBA(iter.fragment.bytes, iter.fragment.size, &size.x(), &size.y());
            if (!data) {
                throw ImageLoadError{"Failed to decode webp frame."};
            }

            ScopeGuard dataGuard{[data] { WebPFree(data); }};

            ImageData& resultData = result.emplace_back();
            if (exifAttributes) {
                resultData.attributes.emplace_back(exifAttributes.value());
            }

            resultData.channels = makeRgbaInterleavedChannels(numChannels, numChannels == 4, size);
            resultData.hasPremultipliedAlpha = false;
            resultData.partName = fmt::format("frames.{}", frameIdx);

            ++frameIdx;

            if (iccProfile) {
                try {
                    // Color space conversion from float to float is faster than u8 to float, hence we convert first.
                    const size_t numSamples = (size_t)size.x() * size.y() * numChannels;
                    vector<float> floatData(numSamples);
                    co_await toFloat32(data, numChannels, floatData.data(), numChannels, size, true, priority);
                    co_await toLinearSrgbPremul(
                        *iccProfile,
                        size,
                        numColorChannels,
                        EAlphaKind::Straight,
                        EPixelFormat::F32,
                        (uint8_t*)floatData.data(),
                        resultData.channels.front().data(),
                        priority
                    );

                    resultData.hasPremultipliedAlpha = true;
                    continue;
                } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC profile: {}", e.what()); }
            }

            co_await toFloat32<uint8_t, true>(
                (uint8_t*)data, numChannels, resultData.channels.front().data(), 4, size, numChannels == 4, priority
            );
        } while (WebPDemuxNextFrame(&iter));
        WebPDemuxReleaseIterator(&iter);
    }

    // If there's just one frame in this webp, there's no need to give it a part name.
    if (result.size() == 1) {
        result.front().partName = "";
    }

    co_return result;
}

} // namespace tev
