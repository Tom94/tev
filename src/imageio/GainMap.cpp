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

#include <tev/Common.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/Ifd.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/IsoGainMapMetadata.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<void> preprocessAndApplyAppleGainMap(ImageData& image, ImageData& gainMap, const optional<Ifd>& amn, bool shallApply, int priority) {
    if (image.channels.empty() || gainMap.channels.empty()) {
        tlog::warning() << "ISO gain map: image or gain map has no channels. Skipping gain map application.";
        co_return;
    }

    // Apply gain map per https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos

    tlog::debug() << "Apple gain map: linearizing and resizing";

    // First: linearize per the spec, then resize to image size
    const auto gainmapSize = gainMap.channels[0].size();
    const size_t gainmapNumPixels = (size_t)gainmapSize.x() * gainmapSize.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        gainmapNumPixels,
        gainmapNumPixels * gainMap.channels.size(),
        [&](int i) {
            for (int c = 0; c < (int)gainMap.channels.size(); ++c) {
                // NOTE: The docs (above link) say to use the Rec.709 transfer function here, but comparisons with ISO gain maps indicate
                // that the gain maps are actually encoded with the sRGB transfer function.
                // const float gain = ituth273::invTransferComponent(ituth273::ETransfer::BT709, gainMap.channels[gainmapChannel].at(i));
                gainMap.channels[c].setAt(i, toLinear(gainMap.channels[c].at(i)));
            }
        },
        priority
    );

    const auto size = image.channels[0].size();

    co_await ImageLoader::resizeImageData(gainMap, size, priority);

    TEV_ASSERT(size == gainMap.channels[0].size(), "Image and gain map must have the same size.");

    // If we don't actually want to apply the gain map, we should still have done the linearization and resizing above for display of the
    // gain map itself in tev.
    if (!shallApply) {
        co_return;
    }

    // 0.0 and 8.0 result in the weakest effect. They are a sane default; see https://developer.apple.com/forums/thread/709331
    float maker33 = 0.0f;
    float maker48 = 8.0f;

    if (amn.has_value()) {
        maker33 = amn->tryGet<float>(33).value_or(maker33);
        maker48 = amn->tryGet<float>(48).value_or(maker48);
    }

    float stops;
    if (maker33 < 1.0f) {
        if (maker48 <= 0.01f) {
            stops = -20.0f * maker48 + 1.8f;
        } else {
            stops = -0.101f * maker48 + 1.601f;
        }
    } else {
        if (maker48 <= 0.01f) {
            stops = -70.0f * maker48 + 3.0f;
        } else {
            stops = -0.303f * maker48 + 2.303f;
        }
    }

    const float headroom = pow(2.0f, std::max(stops, 0.0f));
    tlog::debug() << fmt::format("Apple gain map: derived headroom {} from maker note entries #33={} and #48={}.", headroom, maker33, maker48);

    const int numImageChannels = (int)image.channels.size();
    const int numGainMapChannels = (int)gainMap.channels.size();

    if (numGainMapChannels > 1) {
        tlog::warning() << "Apple gain map: should only have one channel. Attempting to apply multi-channel gain map.";
    }

    int alphaChannelIndex = -1;
    for (int c = 0; c < numImageChannels; ++c) {
        bool isAlpha = Channel::isAlpha(image.channels[c].name());
        if (isAlpha) {
            if (alphaChannelIndex != -1) {
                tlog::warning() << fmt::format(
                    "Apple gain map: image has multiple alpha channels, using the first one: {}", image.channels[alphaChannelIndex].name()
                );
                continue;
            }

            alphaChannelIndex = c;
        }
    }

    const size_t numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * numImageChannels,
        [&](size_t i) {
            for (int c = 0; c < numImageChannels; ++c) {
                if (c == alphaChannelIndex) {
                    continue;
                }

                const int gainmapChannel = std::min(c, numGainMapChannels - 1);

                const float sdr = image.channels[c].at(i);
                const float gain = gainMap.channels[gainmapChannel].at(i);

                image.channels[c].setAt(i, sdr * (1.0f + (headroom - 1.0f) * gain));
            }
        },
        priority
    );

    co_return;
}

Task<void> preprocessAndApplyIsoGainMap(
    ImageData& image,
    ImageData& gainMap,
    const IsoGainMapMetadata& metadata,
    const optional<chroma_t>& baseChroma,
    const optional<chroma_t>& altChroma,
    bool shallApply,
    int priority
) {
    if (image.channels.empty() || gainMap.channels.empty()) {
        tlog::warning() << "ISO gain map: image or gain map has no channels. Skipping gain map application.";
        co_return;
    }

    // Apply gain map per https://www.iso.org/standard/86775.html (paywalled, unfortunately)

    tlog::debug() << "ISO gain map: undoing gamma, unnormalizing, and resizing";

    const float targetHeadroom = metadata.alternateHdrHeadroom(); // Assume we have all headroom available
    const float weight = std::copysign(
        clamp((targetHeadroom - metadata.baseHdrHeadroom()) / (metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()), 0.0f, 1.0f),
        metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()
    );

    // Per the spec, unnormalize and then resize (in log space) to image size
    const auto gainmapSize = gainMap.channels[0].size();
    const size_t gainmapNumPixels = (size_t)gainmapSize.x() * gainmapSize.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        gainmapNumPixels,
        gainmapNumPixels * gainMap.channels.size(),
        [&](int i) {
            for (int c = 0; c < (int)gainMap.channels.size(); ++c) {
                const float val = gainMap.channels[c].at(i);

                const float logRecovery = copysign(std::pow(abs(val), 1.0f / metadata.gainMapGamma()[c]), val);
                const float logBoost = metadata.gainMapMin()[c] * (1.0f - logRecovery) + metadata.gainMapMax()[c] * logRecovery;

                gainMap.channels[c].setAt(i, logBoost);
            }
        },
        priority
    );

    const auto size = image.channels[0].size();

    co_await ImageLoader::resizeImageData(gainMap, size, priority);

    TEV_ASSERT(size == gainMap.channels[0].size(), "Image and gain map must have the same size.");

    const int numImageChannels = (int)image.channels.size();
    const int numGainMapChannels = (int)gainMap.channels.size();

    int alphaChannelIndex = -1;
    if (Channel::isAlpha(image.channels.back().name())) {
        alphaChannelIndex = image.channels.size() - 1;
    }

    const int numColorChannels = std::min(alphaChannelIndex == -1 ? numImageChannels : (numImageChannels - 1), 3);

    // Before applying the gainmap, convert the image to the appropriate color space. Fall back to base chroma if alt chroma requested but
    // not given (image should have been left in base chroma in that case).
    const auto& chroma = metadata.useBaseColorSpace() ? baseChroma : (altChroma ? altChroma : baseChroma);

    if (chroma) {
        tlog::debug() << fmt::format("ISO gain map: converting image to chroma '{}' prior to application", *chroma);

        const auto rec709ToChroma = convertColorspaceMatrix(rec709Chroma(), *chroma, image.renderingIntent);
        const auto imageToChroma = rec709ToChroma * image.toRec709;

        // NOTE: the color conversion internally updates image.toRec709 accordingly
        co_await image.applyColorConversion(imageToChroma, priority);
    }

    // If we don't actually want to apply the gain map, we should still have done the linearization and resizing above for display of the
    // gain map itself in tev.
    if (weight <= 0.0f || !shallApply) {
        co_return; // We'd like the image to be HDR. If the gainmap describes the HDR->SDR direction, we don't need to do anything.
    }

    tlog::debug() << fmt::format(
        "ISO gain map: applying with baseHdrHeadroom={} altHdrHeadroom={} targetHeadroom={} weight={}",
        metadata.baseHdrHeadroom(),
        metadata.alternateHdrHeadroom(),
        targetHeadroom,
        weight
    );

    // Actual gainmap application
    const size_t numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * numColorChannels,
        [&](size_t i) {
            for (int c = 0; c < numColorChannels; ++c) {
                const int gainmapChannel = std::min(c, numGainMapChannels - 1);

                const float logBoost = gainMap.channels[gainmapChannel].at(i);

                const float sdr = image.channels[c].at(i);
                const float hdr = (sdr + metadata.baseOffset()[c]) * exp2f(logBoost * weight) - metadata.alternateOffset()[c];

                image.channels[c].setAt(i, hdr);
            }
        },
        priority
    );

    co_return;
}

} // namespace tev
