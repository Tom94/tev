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

GainmapHeadroom::GainmapHeadroom(string_view str) {
    if (str.ends_with("%")) {
        unit = EUnit::Percent;
        value = stof(string{str.substr(0, str.size() - 1)}) / 100.0f;
    } else {
        unit = EUnit::Stops;
        value = stof(string{str});
    }
}

string GainmapHeadroom::toString() const {
    if (unit == EUnit::Percent) {
        return fmt::format("{}%", value * 100.0f);
    } else {
        return fmt::format("{} stops", value);
    }
}

static vector<Channel*> getRgbOrLuminanceChannels(ImageData& image) {
    image.updateLayers();

    Channel* r = nullptr;
    Channel* g = nullptr;
    Channel* b = nullptr;

    for (auto& layer : image.layers) {
        if (((r = image.mutableChannel(layer + "R")) && (g = image.mutableChannel(layer + "G")) && (b = image.mutableChannel(layer + "B"))) ||
            ((r = image.mutableChannel(layer + "r")) && (g = image.mutableChannel(layer + "g")) && (b = image.mutableChannel(layer + "b")))) {
            return {r, g, b};
        } else if ((r = image.mutableChannel(layer + "L")) || (r = image.mutableChannel(layer + "l")) ||
                   (r = image.mutableChannel(layer + "Y")) || (r = image.mutableChannel(layer + "y"))) {
            return {r};
        }
    }

    return {};
}

Task<void> preprocessAndApplyAppleGainMap(
    ImageData& image, ImageData& gainMap, const optional<Ifd>& amn, const GainmapHeadroom& targetHeadroom, int priority
) {
    const auto imageChannels = getRgbOrLuminanceChannels(image);
    auto gainMapChannels = getRgbOrLuminanceChannels(gainMap);

    if (imageChannels.empty() || gainMapChannels.empty()) {
        tlog::warning() << "Apple gain map: image or gain map has no channels. Skipping gain map application.";
        co_return;
    }

    // Apply gain map per https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos

    tlog::debug() << "Apple gain map: linearizing and resizing";

    // First: linearize per the spec, then resize to image size
    const auto gainmapSize = gainMapChannels.front()->size();
    const size_t gainmapNumPixels = (size_t)gainmapSize.x() * gainmapSize.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        gainmapNumPixels,
        gainmapNumPixels * gainMapChannels.size(),
        [&](int i) {
            for (int c = 0; c < (int)gainMapChannels.size(); ++c) {
                // NOTE: The docs (above link) say to use the Rec.709 transfer function here, but comparisons with ISO gain maps indicate
                // that the gain maps are actually encoded with the sRGB transfer function.
                // const float gain = ituth273::invTransferComponent(ituth273::ETransfer::BT709, gainMapChannels[gainmapChannel].at(i));
                gainMapChannels[c]->setAt(i, toLinear(gainMapChannels[c]->at(i)));
            }
        },
        priority
    );

    const auto size = imageChannels.front()->size();

    co_await ImageLoader::resizeImageData(gainMap, size, nullopt, priority);

    // Re-fetch channels after resize
    gainMapChannels = getRgbOrLuminanceChannels(gainMap);
    TEV_ASSERT(!gainMapChannels.empty(), "Gain map must have at least one channel after resize.");
    TEV_ASSERT(
        size == gainMapChannels.front()->size(), "Image and gain map must have the same size. ({}!={})", size, gainMapChannels.front()->size()
    );

    // 0.0 and 8.0 result in the weakest effect. They are a sane default; see https://developer.apple.com/forums/thread/709331
    float maker33 = 0.0f;
    float maker48 = 8.0f;

    if (amn.has_value()) {
        tlog::debug() << "Apple gain map: found maker note data. Attempting to read maker notes #33 and #48 for gain map weight calculation...";
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
        exp2f(clamp(stops * targetHeadroom.value, 0.0f, stops)) :
        exp2f(clamp(stops, 0.0f, targetHeadroom.value));

    // If we don't actually want to apply the gain map, we should still have done the linearization and resizing above for display of the
    // gain map itself in tev.
    if (headroom <= 1.0f) {
        tlog::debug() << "Apple gain map: target headroom <= 1.0, skipping gain map application.";
        co_return;
    }

    tlog::debug() << fmt::format(
        "Apple gain map: derived weight {} from headroom {} and maker note #33={} #48={}", headroom, targetHeadroom.toString(), maker33, maker48
    );

    if (gainMapChannels.size() > 1) {
        tlog::warning() << "Apple gain map: should only have one channel. Attempting to apply multi-channel gain map.";
    }

    const size_t numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * imageChannels.size(),
        [&](size_t i) {
            for (size_t c = 0; c < imageChannels.size(); ++c) {
                const size_t gainmapChannel = std::min(c, gainMapChannels.size() - 1);

                const float sdr = imageChannels[c]->at(i);
                const float gain = gainMapChannels[gainmapChannel]->at(i);

                imageChannels[c]->setAt(i, sdr * (1.0f + (headroom - 1.0f) * gain));
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
    const auto imageChannels = getRgbOrLuminanceChannels(image);
    auto gainMapChannels = getRgbOrLuminanceChannels(gainMap);

    if (imageChannels.empty() || gainMapChannels.empty()) {
        tlog::warning() << "ISO gain map: image or gain map has no channels. Skipping gain map application.";
        co_return;
    }

    // Apply gain map per https://www.iso.org/standard/86775.html (paywalled, unfortunately)

    tlog::debug() << "ISO gain map: undoing gamma, unnormalizing, and resizing";

    const float targetHeadroomStops = targetHeadroom.unit == GainmapHeadroom::EUnit::Percent ?
        metadata.baseHdrHeadroom() + targetHeadroom.value * (metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()) :
        targetHeadroom.value;

    const float weight = copysign(
        clamp((targetHeadroomStops - metadata.baseHdrHeadroom()) / (metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()), 0.0f, 1.0f),
        metadata.alternateHdrHeadroom() - metadata.baseHdrHeadroom()
    );

    // Per the spec, unnormalize and then resize (in log space) to image size
    const auto gainmapSize = gainMapChannels.front()->size();
    const size_t gainmapNumPixels = (size_t)gainmapSize.x() * gainmapSize.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        gainmapNumPixels,
        gainmapNumPixels * gainMapChannels.size(),
        [&](int i) {
            for (int c = 0; c < (int)gainMapChannels.size(); ++c) {
                const float val = gainMapChannels[c]->at(i);

                const float logRecovery = copysign(pow(abs(val), 1.0f / metadata.gainMapGamma()[c]), val);
                const float logBoost = metadata.gainMapMin()[c] * (1.0f - logRecovery) + metadata.gainMapMax()[c] * logRecovery;

                gainMapChannels[c]->setAt(i, logBoost);
            }
        },
        priority
    );

    const auto size = imageChannels.front()->size();

    co_await ImageLoader::resizeImageData(gainMap, size, nullopt, priority);

    // Re-fetch channels after resize
    gainMapChannels = getRgbOrLuminanceChannels(gainMap);
    TEV_ASSERT(!gainMapChannels.empty(), "Gain map must have at least one channel after resize.");
    TEV_ASSERT(
        size == gainMapChannels.front()->size(), "Image and gain map must have the same size. ({}!={})", size, gainMapChannels.front()->size()
    );

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
    if (weight == 0.0f) {
        tlog::debug() << "ISO gain map: weight is 0, skipping gain map application.";
        co_return;
    }

    tlog::debug() << fmt::format(
        "ISO gain map: applying with baseHdrHeadroom={} altHdrHeadroom={} targetHeadroom={} weight={}",
        metadata.baseHdrHeadroom(),
        metadata.alternateHdrHeadroom(),
        targetHeadroomStops,
        weight
    );

    // Actual gainmap application
    const size_t numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * imageChannels.size(),
        [&](size_t i) {
            for (size_t c = 0; c < imageChannels.size(); ++c) {
                const int gainmapChannel = std::min(c, gainMapChannels.size() - 1);

                const float logBoost = gainMapChannels[gainmapChannel]->at(i);

                const float sdr = imageChannels[c]->at(i);
                const float hdr = (sdr + metadata.baseOffset()[c]) * exp2f(logBoost * weight) - metadata.alternateOffset()[c];

                imageChannels[c]->setAt(i, hdr);
            }
        },
        priority
    );

    co_return;
}

} // namespace tev
