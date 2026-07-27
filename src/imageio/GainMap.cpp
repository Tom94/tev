/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Colors.h>
#include <tev/Common.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/Ifd.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/IsoGainMapMetadata.h>

using namespace nanogui;
using namespace std;

namespace tev {

GainmapHeadroom::GainmapHeadroom(string_view str) {
    if (str.ends_with("%")) {
        unit = EUnit::Percent;
        if (!fromChars(str.substr(0, str.size() - 1), value)) {
            throw runtime_error{fmt::format("Invalid headroom percentage: {}", str)};
        }

        value /= 100.0f;
    } else {
        unit = EUnit::Stops;
        if (!fromChars(str, value)) {
            throw runtime_error{fmt::format("Invalid headroom stops: {}", str)};
        }
    }
}

string GainmapHeadroom::toString() const {
    if (unit == EUnit::Percent) {
        return fmt::format("{}%", value * 100.0f);
    } else {
        return fmt::format("{} stops", value);
    }
}

static optional<MultiChannelView<float>> getRgbOrLuminanceChannels(ImageData& image) {
    image.updateLayers();

    Channel* r = nullptr;
    Channel* g = nullptr;
    Channel* b = nullptr;

    for (auto& layer : image.layers) {
        if (((r = image.mutableChannel(layer + "R")) && (g = image.mutableChannel(layer + "G")) && (b = image.mutableChannel(layer + "B"))) ||
            ((r = image.mutableChannel(layer + "r")) && (g = image.mutableChannel(layer + "g")) && (b = image.mutableChannel(layer + "b")))) {
            return MultiChannelView<float>{
                vector{r->view<float>(), g->view<float>(), b->view<float>()}
            };
        } else if (
            (r = image.mutableChannel(layer + "L")) || (r = image.mutableChannel(layer + "l")) || (r = image.mutableChannel(layer + "Y")) ||
            (r = image.mutableChannel(layer + "y"))
        ) {
            return {r->view<float>()};
        }
    }

    return nullopt;
}

Task<void> preprocessAndApplyAppleGainMap(
    ImageData& image, ImageData& gainMap, const optional<Ifd>& amn, const GainmapHeadroom& targetHeadroom, int priority
) {
    auto optImageChannels = getRgbOrLuminanceChannels(image);
    auto optGainMapChannels = getRgbOrLuminanceChannels(gainMap);

    if (!optImageChannels || !optGainMapChannels) {
        tlog::warning("Apple gain map: image or gain map has no channels. Skipping gain map application.");
        co_return;
    }

    auto imageChannels = *optImageChannels;
    auto gainMapChannels = *optGainMapChannels;

    // Apply gain map per https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos

    tlog::debug("Apple gain map: linearizing and resizing");

    // First: linearize per the spec, then resize to image size
    const auto gainmapSize = gainMapChannels.size();
    const size_t gainmapNumPixels = posProd(gainmapSize);
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        gainmapNumPixels,
        gainmapNumPixels * gainMapChannels.nChannels() * ituth273::approxCost(ituth273::ETransfer::SRGB),
        [&]<class B>(size_t i) {
            for (size_t c = 0; c < gainMapChannels.nChannels(); ++c) {
                // NOTE: The docs (above link) say to use the Rec.709 transfer function here, but comparisons with ISO gain maps indicate
                // that the gain maps are actually encoded with the sRGB transfer function.
                // const float gain = ituth273::bt709ToLinear(gainMapChannels[gainmapChannel].at(i));
                storeChannel<B>(gainMapChannels, c, i, ituth273::srgbToLinear(loadChannel<B>(gainMapChannels, c, i)));
            }
        },
        priority
    );

    const auto size = imageChannels.size();

    co_await ImageLoader::resizeImageData(gainMap, size, nullopt, priority);

    // Re-fetch channels after resize
    optGainMapChannels = getRgbOrLuminanceChannels(gainMap);
    TEV_ASSERT(optGainMapChannels, "Gain map must have at least one channel after resize.");
    gainMapChannels = *optGainMapChannels;
    TEV_ASSERT(size == gainMapChannels.size(), "Image and gain map must have the same size. ({}!={})", size, gainMapChannels.size());

    // Apple gain maps are always assumed to be in the image's color space. (Technically an irrelevant detail, because they're also assumed
    // to be monochromatic, but we'll set the metadata to generalize just in case, analogously to ISO gain maps.)
    gainMap.toRec709 = image.toRec709;

    // 0.0 and 8.0 result in the weakest effect. They are a sane default; see https://developer.apple.com/forums/thread/709331
    float maker33 = 0.0f;
    float maker48 = 8.0f;

    if (amn.has_value()) {
        tlog::debug("Apple gain map: found maker note data. Attempting to read maker notes #33 and #48 for gain map weight calculation...");
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

    const float headroom = targetHeadroom.unit == GainmapHeadroom::EUnit::Percent ?
        std::exp2(clamp(stops * targetHeadroom.value, 0.0f, stops)) :
        std::exp2(clamp(stops, 0.0f, targetHeadroom.value));

    // If we don't actually want to apply the gain map, we should still have done the linearization and resizing above for display of the
    // gain map itself in tev.
    if (headroom <= 1.0f) {
        tlog::debug("Apple gain map: target headroom <= 1.0, skipping gain map application.");
        co_return;
    }

    tlog::debug(
        "Apple gain map: derived weight {} from headroom {} and maker note #33={} #48={}", headroom, targetHeadroom.toString(), maker33, maker48
    );

    if (gainMapChannels.nChannels() > 1) {
        tlog::warning("Apple gain map: should only have one channel. Attempting to apply multi-channel gain map.");
    }

    const size_t numPixels = posProd(size);
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        numPixels,
        numPixels * imageChannels.nChannels(),
        [&]<class B>(size_t i) {
            for (size_t c = 0; c < imageChannels.nChannels(); ++c) {
                const size_t gainMapChannel = std::min(c, gainMapChannels.nChannels() - 1);

                const auto sdr = loadChannel<B>(imageChannels, c, i);
                const auto gain = loadChannel<B>(gainMapChannels, gainMapChannel, i);

                storeChannel<B>(imageChannels, c, i, sdr * (1.0f + (headroom - 1.0f) * gain));
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
    const GainmapHeadroom& targetHeadroom,
    int priority
) {
    auto optImageChannels = getRgbOrLuminanceChannels(image);
    auto optGainMapChannels = getRgbOrLuminanceChannels(gainMap);

    if (!optImageChannels || !optGainMapChannels) {
        tlog::warning("ISO gain map: image or gain map has no channels. Skipping gain map application.");
        co_return;
    }

    auto imageChannels = *optImageChannels;
    auto gainMapChannels = *optGainMapChannels;

    // Apply gain map per https://www.iso.org/standard/86775.html (paywalled, unfortunately)

    tlog::debug("ISO gain map: undoing gamma, unnormalizing, and resizing");

    const float targetHeadroomStops = targetHeadroom.unit == GainmapHeadroom::EUnit::Percent ?
        metadata.baseHdrHeadroom() + targetHeadroom.value * (metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()) :
        targetHeadroom.value;

    const float weight = copysign(
        clamp((targetHeadroomStops - metadata.baseHdrHeadroom()) / (metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()), 0.0f, 1.0f),
        metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()
    );

    // Per the spec, unnormalize and then resize (in log space) to image size
    const auto gainmapSize = gainMapChannels.size();
    const size_t gainmapNumPixels = posProd(gainmapSize);
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        gainmapNumPixels,
        gainmapNumPixels * gainMapChannels.nChannels(),
        [&]<class B>(size_t i) {
            for (size_t c = 0; c < gainMapChannels.nChannels(); ++c) {
                const auto val = loadChannel<B>(gainMapChannels, c, i);

                const auto logRecovery = xsimd::copysign(fastPow(xsimd::abs(val), B{1.0f / metadata.gainMapGamma()[c]}), val);
                const auto logBoost = metadata.gainMapMin()[c] * (1.0f - logRecovery) + metadata.gainMapMax()[c] * logRecovery;

                storeChannel<B>(gainMapChannels, c, i, logBoost);
            }
        },
        priority
    );

    const auto size = imageChannels.size();

    co_await ImageLoader::resizeImageData(gainMap, size, nullopt, priority);

    // Re-fetch channels after resize
    optGainMapChannels = getRgbOrLuminanceChannels(gainMap);
    TEV_ASSERT(optGainMapChannels, "Gain map must have at least one channel after resize.");
    gainMapChannels = *optGainMapChannels;
    TEV_ASSERT(size == gainMapChannels.size(), "Image and gain map must have the same size. ({}!={})", size, gainMapChannels.size());

    // Before applying the gainmap, convert the image to the appropriate color space. Fall back to base chroma if alt chroma requested but
    // not given (image should have been left in base chroma in that case). Gainmap is assumed to be in that color space as well.
    const auto& chroma = metadata.useBaseColorSpace() ? baseChroma : (altChroma ? altChroma : baseChroma);

    if (chroma) {
        tlog::debug("ISO gain map: converting image to chroma '{}' prior to application", *chroma);

        const auto rec709ToChroma = convertColorspaceMatrix(rec709Chroma(), *chroma, image.renderingIntent);
        const auto imageToChroma = rec709ToChroma * image.toRec709;

        // NOTE: the color conversion internally updates image.toRec709 accordingly
        co_await image.applyColorConversion(imageToChroma, priority);
    }

    // The image and gain map are not in the gainmap application color space. Reflect this in the gainmap's meatadata.
    gainMap.toRec709 = image.toRec709;

    // If we don't actually want to apply the gain map, we should still have done the linearization and resizing above for display of the
    // gain map itself in tev. The color space conversion is also necessary to ensure that the gainmap channels (which are expressed in
    // terms of `chroma` get correctly converted to rec.709 once concatenated to the main image).
    if (weight == 0.0f) {
        tlog::debug("ISO gain map: weight is 0, skipping gain map application.");
        co_return;
    }

    tlog::debug(
        "ISO gain map: applying with baseHdrHeadroom={} altHdrHeadroom={} targetHeadroom={} weight={}",
        metadata.baseHdrHeadroom(),
        metadata.alternateHdrHeadroom(),
        targetHeadroomStops,
        weight
    );

    // Actual gainmap application
    const size_t numPixels = posProd(size);
    co_await simdParallelFor(
        ThreadPool::global(),
        0uz,
        numPixels,
        numPixels * imageChannels.nChannels(),
        [&]<class B>(size_t i) {
            for (size_t c = 0; c < imageChannels.nChannels(); ++c) {
                const int gainMapChannel = std::min(c, gainMapChannels.nChannels() - 1);

                const auto sdr = loadChannel<B>(imageChannels, c, i);
                const auto logBoost = loadChannel<B>(gainMapChannels, gainMapChannel, i);

                const auto hdr = (sdr + metadata.baseOffset()[c]) * fastExp2(logBoost * weight) - metadata.alternateOffset()[c];

                storeChannel<B>(imageChannels, c, i, hdr);
            }
        },
        priority
    );

    co_return;
}

} // namespace tev
