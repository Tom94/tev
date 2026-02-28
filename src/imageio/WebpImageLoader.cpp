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

#include <tev/Box.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/WebpImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <webp/decode.h>
#include <webp/demux.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> WebpImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    char magic[16] = {0};
    iStream.read(magic, sizeof(magic));
    if (!iStream || strncmp(magic, "RIFF", 4) != 0 || strncmp(magic + 8, "WEBP", 4) != 0) {
        throw FormatNotSupported{"File is not a webp image."};
    }

    iStream.seekg(0, ios::end);
    const auto fileSize = iStream.tellg();
    iStream.seekg(0, ios::beg);

    HeapArray<uint8_t> buffer(fileSize);
    iStream.read((char*)buffer.data(), fileSize);

    WebPData data = {buffer.data(), buffer.size()};
    WebPDemuxer* demux = WebPDemux(&data);
    if (!demux) {
        throw ImageLoadError{"Failed to demux webp image."};
    }

    ScopeGuard demuxGuard{[demux] { WebPDemuxDelete(demux); }};

    const uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);

    WebPChunkIterator chunkIter;

    HeapArray<uint8_t> iccProfileData;
    if (flags & ICCP_FLAG) {
        if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunkIter)) {
            ScopeGuard chunkGuard{[&chunkIter] { WebPDemuxReleaseChunkIterator(&chunkIter); }};
            try {
                tlog::debug() << "Found ICC color profile.";
                iccProfileData = HeapArray<uint8_t>(chunkIter.chunk.size);
                memcpy(iccProfileData.data(), chunkIter.chunk.bytes, chunkIter.chunk.size);
            } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to create ICC color profile: {}", e.what()); }
        } else {
            tlog::warning() << "Failed to get ICCP chunk from webp image, despite flag being set.";
        }
    }

    vector<AttributeNode> attributes;
    if (flags & EXIF_FLAG) {
        if (WebPDemuxGetChunk(demux, "EXIF", 1, &chunkIter)) {
            ScopeGuard chunkGuard{[&chunkIter] { WebPDemuxReleaseChunkIterator(&chunkIter); }};

            try {
                auto exif = Exif{
                    {chunkIter.chunk.bytes, chunkIter.chunk.size}
                };
                attributes.emplace_back(exif.toAttributes());
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
        } else {
            tlog::warning() << "Failed to get EXIF chunk from webp image, despite flag being set.";
        }
    }

    if (flags & XMP_FLAG) {
        if (WebPDemuxGetChunk(demux, "XMP ", 1, &chunkIter)) {
            ScopeGuard chunkGuard{[&chunkIter] { WebPDemuxReleaseChunkIterator(&chunkIter); }};

            try {
                auto xmp = Xmp{
                    string_view{(const char*)chunkIter.chunk.bytes, chunkIter.chunk.size}
                };
                attributes.emplace_back(xmp.attributes());
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read XMP metadata: {}", e.what()); }
        } else {
            tlog::warning() << "Failed to get XMP chunk from webp image, despite flag being set.";
        }
    }

    const size_t numChannels = 4;
    const size_t numColorChannels = 3;
    const size_t numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
    const bool hasAlpha = numChannels == 4;

    const uint32_t width = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    const uint32_t height = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    Color bgColor = {0.0f, 0.0f, 0.0f, 0.0f};

    const bool isAnimation = (flags & ANIMATION_FLAG) != 0;
    if (isAnimation) {
        const uint32_t bgColor8bit = WebPDemuxGetI(demux, WEBP_FF_BACKGROUND_COLOR);

        // Byte order: BGRA (https://developers.google.com/speed/webp/docs/riff_container#animation)
        const uint8_t* bgColorBytes = (const uint8_t*)&bgColor8bit;

        bgColor = Color{bgColorBytes[2] / 255.0f, bgColorBytes[1] / 255.0f, bgColorBytes[0] / 255.0f, bgColorBytes[3] / 255.0f};
        if (iccProfileData) {
            try {
                auto tmp = bgColor;
                co_await toLinearSrgbPremul(
                    ColorProfile::fromIcc(iccProfileData), {1, 1}, numColorChannels, EAlphaKind::Straight, tmp.data(), bgColor.data(), 4, nullopt, priority
                );
            } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC profile: {}", e.what()); }
        } else {
            for (uint32_t i = 0; i < 3; ++i) {
                bgColor[i] = toLinear(bgColor[i]) * bgColor[3]; // Premultiply alpha
            }
        }
    }

    const Vector2i size{(int)width, (int)height};

    vector<ImageData> result;

    WebPIterator iter;
    size_t frameIdx = 0;
    bool disposed = true;

    // Conservatively allocate enough space such that any frame can be decoded into it.
    const size_t numPixels = (size_t)size.x() * size.y();
    const size_t numSamples = numPixels * numChannels;
    const size_t numInterleavedSamples = numPixels * numInterleavedChannels;
    HeapArray<float> frameData;

    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            Vector2i frameSize;
            uint8_t* const data = WebPDecodeRGBA(iter.fragment.bytes, iter.fragment.size, &frameSize.x(), &frameSize.y());
            if (!data) {
                throw ImageLoadError{"Failed to decode webp frame."};
            }

            const Vector2i frameOffset{iter.x_offset, iter.y_offset};

            const ScopeGuard dataGuard{[data] { WebPFree(data); }};

            ImageData& resultData = result.emplace_back();
            resultData.attributes = attributes;

            // WebP is always 8bit per channel, so we can comfortably use F16 for the decoded data.
            resultData.partName = isAnimation ? fmt::format("frames.{}", frameIdx++) : "";
            resultData.channels = co_await makeRgbaInterleavedChannels(
                numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, resultData.partName, priority
            );
            resultData.hasPremultipliedAlpha = false;

            const auto outView = MultiChannelView<float>{resultData.channels};

            // If our frame fills the entire canvas and is configured to overwrite the canvas (as is the case for static frames / PNGs), we
            // can directly write onto the canvas and not worry about blending.
            const bool directlyOnCanvas = frameOffset == Vector2i{0, 0} && frameSize == size;

            const size_t numFramePixels = (size_t)frameSize.x() * frameSize.y();
            const size_t numInterleavedFrameSamples = numFramePixels * numInterleavedChannels;
            if (!directlyOnCanvas && numInterleavedFrameSamples > frameData.size()) {
                const size_t allocationSize = std::max(numInterleavedFrameSamples, numInterleavedSamples);
                if (allocationSize > numSamples) {
                    tlog::warning() << fmt::format("WebP frame data {} is larger than final image buffer {}. Re-allocating.", frameSize, size);
                }

                frameData = HeapArray<float>(allocationSize);
            }

            const auto dstView = directlyOnCanvas ? outView : MultiChannelView<float>{frameData.data(), numInterleavedChannels, frameSize};

            if (iccProfileData) {
                try {
                    co_await toFloat32(data, numChannels, dstView, hasAlpha, priority);

                    const auto profile = ColorProfile::fromIcc(iccProfileData);
                    co_await toLinearSrgbPremul(
                        profile, hasAlpha ? EAlphaKind::Straight : EAlphaKind::None, dstView, dstView, nullopt, priority
                    );

                    resultData.readMetadataFromIcc(profile);
                } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC profile: {}", e.what()); }
            } else {
                co_await toFloat32<uint8_t, true, true>((uint8_t*)data, numChannels, dstView, hasAlpha, priority);

                resultData.nativeMetadata.chroma = rec709Chroma();
                resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
            }

            // If we did not dispose the previous canvas, we need to blend the current frame onto it. Otherwise, blend onto background. The
            // first frame is always disposed.
            const auto prevCanvas = result.size() > 1 ? optional{MultiChannelView<float>{result.at(result.size() - 2).channels}} : nullopt;
            const bool useBg = disposed || prevCanvas == nullopt;
            disposed = iter.dispose_method == WEBP_MUX_DISPOSE_BACKGROUND;

            if (!directlyOnCanvas || iter.blend_method != WEBP_MUX_NO_BLEND) {
                co_await ThreadPool::global().parallelForAsync<int>(
                    0,
                    size.y(),
                    numSamples,
                    [&](int y) {
                        Vector2i framePos;
                        framePos.y() = y - frameOffset.y();
                        for (int x = 0; x < size.x(); ++x) {
                            framePos.x() = x - frameOffset.x();
                            const bool isInFrame = Box2i{frameSize}.contains(framePos);

                            for (size_t c = 0; c < numChannels; ++c) {
                                const float bg = useBg ? bgColor[c] : (*prevCanvas)[c, x, y];
                                float val = bg;

                                if (isInFrame) {
                                    if (iter.blend_method == WEBP_MUX_NO_BLEND) {
                                        val = dstView[c, framePos.x(), framePos.y()];
                                    } else {
                                        const float alpha = hasAlpha ? dstView[-1, framePos.x(), framePos.y()] : 1.0f;
                                        val = dstView[c, framePos.x(), framePos.y()] + bg * (1.0f - alpha);
                                    }
                                }

                                outView[c, x, y] = val;
                            }
                        }
                    },
                    priority
                );
            }

            resultData.hasPremultipliedAlpha = true;
        } while (WebPDemuxNextFrame(&iter));
        WebPDemuxReleaseIterator(&iter);
    }

    if (result.size() > 1 && !isAnimation) {
        tlog::warning() << "WebP image has multiple frames, but animation flag is not set";
    }

    co_return result;
}

} // namespace tev
