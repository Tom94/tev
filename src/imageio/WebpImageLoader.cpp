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

#include <tev/Box.h>
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

    const uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);

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

    const uint32_t width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    const uint32_t height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    array<float, 4> bgColor = {0.0f, 0.0f, 0.0f, 0.0f};

    if (flags & ANIMATION_FLAG) {
        const uint32_t bgColor8bit = WebPDemuxGetI(demux, WEBP_FF_BACKGROUND_COLOR);

        // Byte order: BGRA (https://developers.google.com/speed/webp/docs/riff_container#animation)
        const uint8_t* bgColorBytes = (const uint8_t*)&bgColor8bit;

        bgColor = {bgColorBytes[2] / 255.0f, bgColorBytes[1] / 255.0f, bgColorBytes[0] / 255.0f, bgColorBytes[3] / 255.0f};
        if (iccProfile) {
            try {
                const auto tmp = bgColor;
                co_await toLinearSrgbPremul(
                    *iccProfile, {1, 1}, 3, EAlphaKind::Straight, EPixelFormat::F32, (uint8_t*)tmp.data(), bgColor.data(), priority
                );
            } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC profile: {}", e.what()); }
        } else {
            for (uint32_t i = 0; i < 3; ++i) {
                bgColor[i] = toLinear(bgColor[i]) * bgColor[3]; // Premultiply alpha
            }
        }
    }

    const Vector2i size{(int)width, (int)height};

    const int numChannels = 4;
    const int numColorChannels = 3;

    vector<ImageData> result;

    WebPIterator iter;
    size_t frameIdx = 0;
    bool disposed = true;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            Vector2i frameSize;
            uint8_t* data = WebPDecodeRGBA(iter.fragment.bytes, iter.fragment.size, &frameSize.x(), &frameSize.y());
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

            const size_t numFramePixels = (size_t)frameSize.x() * frameSize.y();
            const size_t numFrameSamples = numFramePixels * numChannels;

            vector<float> frameData(numFrameSamples);
            if (iccProfile) {
                try {
                    // Color space conversion from float to float is faster than u8 to float, hence we convert first.
                    vector<float> floatData(numFrameSamples);
                    co_await toFloat32(data, numChannels, floatData.data(), numChannels, frameSize, true, priority);
                    co_await toLinearSrgbPremul(
                        *iccProfile,
                        frameSize,
                        numColorChannels,
                        EAlphaKind::Straight,
                        EPixelFormat::F32,
                        (uint8_t*)floatData.data(),
                        frameData.data(),
                        priority
                    );
                } catch (const std::runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC profile: {}", e.what()); }
            } else {
                co_await toFloat32<uint8_t, true, true>(
                    (uint8_t*)data, numChannels, frameData.data(), 4, frameSize, numChannels == 4, priority
                );
            }

            // If we did not dispose the previous canvas, we need to blend the current frame onto it. Otherwise, blend onto background. The
            // first frame is always disposed.
            const float* prevCanvas = result.size() > 1 ? result.at(result.size() - 2).channels.front().data() : nullptr;
            bool useBg = disposed || prevCanvas == nullptr;
            disposed = iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND;

            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                [&](int y) {
                    Vector2i framePos;
                    framePos.y() = y - iter.y_offset;
                    for (int x = 0; x < size.x(); ++x) {
                        const size_t canvasPixelIdx = (size_t)y * size.x() + (size_t)x;
                        framePos.x() = x - iter.x_offset;

                        bool isInFrame = Box2i{frameSize}.contains(framePos);

                        for (int c = 0; c < numChannels; ++c) {
                            const size_t canvasSampleIdx = canvasPixelIdx * numChannels + c;
                            float val;

                            const float bg = useBg ? bgColor[c] : prevCanvas[canvasSampleIdx];
                            if (isInFrame) {
                                const size_t framePixelIdx = (size_t)framePos.y() * frameSize.x() + (size_t)framePos.x();
                                const size_t frameSampleIdx = framePixelIdx * numChannels + c;
                                const size_t frameAlphaIdx = framePixelIdx * numChannels + 3;

                                if (iter.blend_method == WEBP_MUX_NO_BLEND) {
                                    val = frameData[frameSampleIdx];
                                } else {
                                    val = frameData[frameSampleIdx] + bg * (1.0f - frameData[frameAlphaIdx]);
                                }
                            } else {
                                val = bg;
                            }

                            resultData.channels.front().data()[canvasSampleIdx] = val;
                        }
                    }
                },
                priority
            );

            resultData.hasPremultipliedAlpha = true;
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
