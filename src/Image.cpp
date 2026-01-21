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
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/ImageSaver.h>

#include <half.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <istream>
#include <map>
#include <numeric>
#include <unordered_set>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

AttributeNode HdrMetadata::toAttributes() const {
    const auto floatToStringZeroMeansNA = [](float v) {
        if (v <= 0.0f) {
            return string{"n/a"};
        } else {
            return to_string(v);
        }
    };

    AttributeNode root;
    root.name = "HDR metadata";

    AttributeNode& content = root.children.emplace_back();
    content.name = "Content light level";
    content.children.emplace_back(
        AttributeNode{.name = "Best guess white level", .value = floatToStringZeroMeansNA(bestGuessWhiteLevel), .type = "cd/m²", .children = {}}
    );
    content.children.emplace_back(
        AttributeNode{.name = "Max content light level", .value = floatToStringZeroMeansNA(maxCLL), .type = "cd/m²", .children = {}}
    );
    content.children.emplace_back(
        AttributeNode{.name = "Max frame average light level", .value = floatToStringZeroMeansNA(maxFALL), .type = "cd/m²", .children = {}}
    );

    AttributeNode& masteringDisplay = root.children.emplace_back();
    masteringDisplay.name = "Mastering display color volume";
    masteringDisplay.children.emplace_back(
        AttributeNode{.name = "Min luminance", .value = floatToStringZeroMeansNA(masteringMinLum), .type = "cd/m²", .children = {}}
    );
    masteringDisplay.children.emplace_back(
        AttributeNode{.name = "Max luminance", .value = floatToStringZeroMeansNA(masteringMaxLum), .type = "cd/m²", .children = {}}
    );

    masteringDisplay.children.emplace_back(
        AttributeNode{
            .name = "Red primary",
            .value = "(" + floatToStringZeroMeansNA(masteringChroma[0].x()) + ", " + floatToStringZeroMeansNA(masteringChroma[0].y()) + ")",
            .type = "xy",
            .children = {}
        }
    );
    masteringDisplay.children.emplace_back(
        AttributeNode{
            .name = "Green primary",
            .value = "(" + floatToStringZeroMeansNA(masteringChroma[1].x()) + ", " + floatToStringZeroMeansNA(masteringChroma[1].y()) + ")",
            .type = "xy",
            .children = {}
        }
    );
    masteringDisplay.children.emplace_back(
        AttributeNode{
            .name = "Blue primary",
            .value = "(" + floatToStringZeroMeansNA(masteringChroma[2].x()) + ", " + floatToStringZeroMeansNA(masteringChroma[2].y()) + ")",
            .type = "xy",
            .children = {}
        }
    );
    masteringDisplay.children.emplace_back(
        AttributeNode{
            .name = "White point",
            .value = "(" + floatToStringZeroMeansNA(masteringChroma[3].x()) + ", " + floatToStringZeroMeansNA(masteringChroma[3].y()) + ")",
            .type = "xy",
            .children = {}
        }
    );

    return root;
}

vector<string> ImageData::channelsInLayer(string_view layerName) const {
    vector<string> result;

    for (const auto& c : channels) {
        // If the layer name starts at the beginning, and if no other dot is found after the end of the layer name, then we have found a
        // channel of this layer.
        if (c.name().starts_with(layerName)) {
            const auto& channelWithoutLayer = c.name().substr(layerName.length());
            if (channelWithoutLayer.find(".") == string::npos) {
                result.emplace_back(c.name());
            }
        }
    }

    return result;
}

Task<void> ImageData::convertToRec709(int priority) {
    // No need to do anything for identity transforms
    if (toRec709 == Matrix3f{1.0f}) {
        co_return;
    }

    vector<Task<void>> tasks;

    for (const auto& layer : layers) {
        Channel* r = nullptr;
        Channel* g = nullptr;
        Channel* b = nullptr;

        if (!(((r = mutableChannel(layer + "R")) && (g = mutableChannel(layer + "G")) && (b = mutableChannel(layer + "B"))) ||
              ((r = mutableChannel(layer + "r")) && (g = mutableChannel(layer + "g")) && (b = mutableChannel(layer + "b"))))) {
            // No RGB-triplet found
            continue;
        }

        TEV_ASSERT(r && g && b, "RGB triplet of channels must exist.");

        tasks.emplace_back(
            ThreadPool::global().parallelForAsync<size_t>(
                0,
                r->numPixels(),
                r->numPixels() * 3,
                [r, g, b, this](size_t i) {
                    const auto rgb = toRec709 * Vector3f{r->at(i), g->at(i), b->at(i)};
                    r->setAt(i, rgb.x());
                    g->setAt(i, rgb.y());
                    b->setAt(i, rgb.z());
                },
                priority
            )
        );
    }

    co_await awaitAll(tasks);

    // Since the image data is now in Rec709 space, converting to Rec709 is the identity transform.
    toRec709 = Matrix3f{1.0f};
}

Task<void> ImageData::deriveWhiteLevelFromMetadata(int priority) {
    if (toRec709 != Matrix3f{1.0f}) {
        throw ImageModifyError{"Cannot derive white level from metadata before converting to Rec709 color space."};
    }

    if (hdrMetadata.maxCLL <= 0.0f && hdrMetadata.maxFALL <= 0.0f) {
        co_return;
    }

    vector<Task<void>> tasks;

    vector<vector<float>> lumPerLayer(layers.size());

    // This function follows the guidance from https://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=9508136 in that maxCLL corresponds to
    // the 99.99th percentile luminance over the image. An additional complication is that "luminance" may be defined as the maximum RGB
    // value (with BT.2020 primaries) in order to prevent clipping during tone mapping based on maxCLL. However, this interpretation is not
    // condusive to deriving a white level for display (which should be actual cd/m² luminance). The following code therefore computes
    // actual luminance, but leaves a commented-out option to compute maximum RGB BT.2020 instead.

    // const auto toRec2020 = convertColorspaceMatrix(
    //     rec709Chroma(), bt2020Chroma(), ERenderingIntent::AbsoluteColorimetric /* intent doesn't matter because white point is the same */
    // );

    for (size_t i = 0; i < layers.size(); ++i) {
        const auto& layer = layers[i];

        Channel* r = nullptr;
        Channel* g = nullptr;
        Channel* b = nullptr;

        if (!(((r = mutableChannel(layer + "R")) && (g = mutableChannel(layer + "G")) && (b = mutableChannel(layer + "B"))) ||
              ((r = mutableChannel(layer + "r")) && (g = mutableChannel(layer + "g")) && (b = mutableChannel(layer + "b"))))) {
            // No RGB-triplet found
            continue;
        }

        TEV_ASSERT(r && g && b, "RGB triplet of channels must exist.");

        lumPerLayer[i].resize(r->numPixels());

        tasks.emplace_back(
            ThreadPool::global().parallelForAsync<size_t>(
                0,
                lumPerLayer[i].size(),
                lumPerLayer[i].size(),
                [r, g, b, &lumBuf = lumPerLayer[i] /*, &toRec2020*/](size_t px) {
                    // Optional: max RGB in BT.2020 primaries (see comment above)
                    // const auto rgb = toRec2020 * Vector3f{r->at(px), g->at(px), b->at(px)};
                    // const float lum = max({rgb.x(), rgb.y(), rgb.z()});

                    const float lum = 0.2126 * r->at(px) + 0.7152 * g->at(px) + 0.0722 * b->at(px);
                    lumBuf[px] = isfinite(lum) ? lum : 0.0f;
                },
                priority
            )
        );
    }

    co_await awaitAll(tasks);

    for (size_t i = 0; i < layers.size(); ++i) {
        // 99.99th percentile luminance per https://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=9508136
        const size_t n = (size_t)(lumPerLayer[i].size() * 0.9999);
        nth_element(begin(lumPerLayer[i]), begin(lumPerLayer[i]) + n, end(lumPerLayer[i]));

        const float maxLum = lumPerLayer[i][n];
        const float avgLum = accumulate(begin(lumPerLayer[i]), end(lumPerLayer[i]), 0.0f) / lumPerLayer[i].size();

        const float whiteLevelFromMaxCLL = hdrMetadata.maxCLL / maxLum;
        const float whiteLevelFromMaxFALL = hdrMetadata.maxFALL / avgLum;

        if (whiteLevelFromMaxCLL > 0.0f) {
            hdrMetadata.bestGuessWhiteLevel = whiteLevelFromMaxCLL;
        }

        if (whiteLevelFromMaxFALL > 0.0f) {
            hdrMetadata.bestGuessWhiteLevel = whiteLevelFromMaxFALL;
        }

        if (whiteLevelFromMaxFALL > 0 && whiteLevelFromMaxCLL > 0 &&
            abs(whiteLevelFromMaxCLL - whiteLevelFromMaxFALL) / (whiteLevelFromMaxCLL + whiteLevelFromMaxFALL) > 0.01f) {
            tlog::warning() << fmt::format(
                "Derived white levels from maxCLL ({}->{}) and maxFALL ({}->{}) of layer '{}' differ by over 1%.",
                hdrMetadata.maxCLL,
                whiteLevelFromMaxCLL,
                hdrMetadata.maxFALL,
                whiteLevelFromMaxFALL,
                layers[i]
            );
        }

        tlog::debug() << fmt::format("Derived white level of {} from metadata & layer '{}'.", hdrMetadata.bestGuessWhiteLevel, layers[i]);
    }
}

Task<void> ImageData::convertToDesiredPixelFormat(int priority) {
    // All channels sharing the same data buffer must be converted together to avoid multiple conversions of the same data.
    multimap<shared_ptr<Channel::Data>, Channel*> channelsByData;
    for (auto& c : channels) {
        channelsByData.emplace(c.dataBuf(), &c);
    }

    for (auto it = channelsByData.begin(); it != channelsByData.end();) {
        const auto rangeEnd = channelsByData.upper_bound(it->first);

        vector<Channel*> channelsToConvert;
        const Channel* firstChannel = it->second;
        const EPixelFormat targetFormat = firstChannel->desiredPixelFormat();
        const EPixelFormat sourceFormat = firstChannel->pixelFormat();

        bool canConvert = true;
        for (auto it2 = it; it2 != rangeEnd; ++it2) {
            const Channel* c = it2->second;
            if (c->pixelFormat() != sourceFormat || c->desiredPixelFormat() != targetFormat) {
                canConvert = false;

                tlog::warning() << fmt::format(
                    "Channels sharing the same data buffer must have the same source and target pixel format. ({}: {} -> {}, {}: {} -> {})",
                    firstChannel->name(),
                    toString(sourceFormat),
                    toString(targetFormat),
                    c->name(),
                    toString(c->pixelFormat()),
                    toString(c->desiredPixelFormat())
                );
            }
        }

        if (!canConvert || sourceFormat == targetFormat) {
            it = rangeEnd;
            continue;
        }

        shared_ptr<Channel::Data> data = it->first;

        const size_t nSamples = data->size() / nBytes(sourceFormat);
        Channel::Data convertedData(nSamples * nBytes(targetFormat));

        const uint8_t* const src = data->data();
        uint8_t* const dst = convertedData.data();

        const auto typedConvert = [nSamples, priority](const auto* typedSrc, auto* typedDst) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<size_t>(
                0, nSamples, nSamples, [typedSrc, typedDst](size_t i) { typedDst[i] = typedSrc[i]; }, priority
            );
        };

        const auto typedSrcConvert = [targetFormat, dst, &typedConvert](const auto* typedSrc) -> Task<void> {
            switch (targetFormat) {
                case EPixelFormat::U8: co_await typedConvert(typedSrc, (uint8_t*)dst); break;
                case EPixelFormat::U16: co_await typedConvert(typedSrc, (uint16_t*)dst); break;
                case EPixelFormat::F16: co_await typedConvert(typedSrc, (half*)dst); break;
                case EPixelFormat::F32: co_await typedConvert(typedSrc, (float*)dst); break;
            }
        };

        switch (sourceFormat) {
            case EPixelFormat::U8: co_await typedSrcConvert((const uint8_t*)src); break;
            case EPixelFormat::U16: co_await typedSrcConvert((const uint16_t*)src); break;
            case EPixelFormat::F16: co_await typedSrcConvert((const half*)src); break;
            case EPixelFormat::F32: co_await typedSrcConvert((const float*)src); break;
        }

        for (auto it2 = it; it2 != rangeEnd; ++it2) {
            Channel* c = it2->second;
            c->setPixelFormat(targetFormat);
            c->setOffset(c->offset() / nBytes(sourceFormat) * nBytes(targetFormat));
            c->setStride(c->stride() / nBytes(sourceFormat) * nBytes(targetFormat));
        }

        *data = std::move(convertedData);

        tlog::debug()
            << fmt::format("Converted {} channels from {} to {}.", distance(it, rangeEnd), toString(sourceFormat), toString(targetFormat));

        it = rangeEnd;
    }
}

void ImageData::alphaOperation(const function<void(Channel&, const Channel&)>& func) {
    for (const auto& layer : layers) {
        string alphaChannelName = layer + "A";

        if (!hasChannel(alphaChannelName)) {
            continue;
        }

        const Channel* alphaChannel = channel(alphaChannelName);
        for (auto& channelName : channelsInLayer(layer)) {
            if (channelName != alphaChannelName) {
                func(*mutableChannel(channelName), *alphaChannel);
            }
        }
    }
}

Task<void> ImageData::multiplyAlpha(int priority) {
    if (hasPremultipliedAlpha) {
        throw ImageModifyError{"Can't multiply with alpha twice."};
    }

    vector<Task<void>> tasks;
    alphaOperation([&](Channel& target, const Channel& alpha) { tasks.emplace_back(target.multiplyWithAsync(alpha, priority)); });
    co_await awaitAll(tasks);

    hasPremultipliedAlpha = true;
}

Task<void> ImageData::unmultiplyAlpha(int priority) {
    if (!hasPremultipliedAlpha) {
        throw ImageModifyError{"Can't divide by alpha twice."};
    }

    vector<Task<void>> tasks;
    alphaOperation([&](Channel& target, const Channel& alpha) { tasks.emplace_back(target.divideByAsync(alpha, priority)); });
    co_await awaitAll(tasks);

    hasPremultipliedAlpha = false;
}

Task<void> ImageData::orientToTopLeft(int priority) {
    if (orientation == EOrientation::TopLeft) {
        co_return;
    }

    bool swapAxes = orientation >= EOrientation::LeftTop;

    struct DataDesc {
        EPixelFormat pixelFormat;
        shared_ptr<Channel::Data> data;
        Vector2i size;

        struct Hash {
            size_t operator()(const DataDesc& interval) const { return hash<shared_ptr<Channel::Data>>()(interval.data); }
        };

        bool operator==(const DataDesc& other) const {
            return pixelFormat == other.pixelFormat && data == other.data && size == other.size;
        }
    };

    unordered_set<DataDesc, DataDesc::Hash> channelData;
    for (auto& c : channels) {
        // TODO: ensure channel data is interleaved if multiple channels share the same data buffer

        channelData.insert({c.pixelFormat(), c.dataBuf(), c.size()});
        if (swapAxes) {
            c.setSize({c.size().y(), c.size().x()});
        }
    }

    for (auto& c : channelData) {
        co_await tev::orientToTopLeft(c.pixelFormat, *c.data, c.size, orientation, priority);
    }

    if (dataWindow.isValid()) {
        dataWindow = applyOrientation(orientation, dataWindow);
    }

    if (displayWindow.isValid()) {
        displayWindow = applyOrientation(orientation, displayWindow);
    }

    orientation = EOrientation::TopLeft;
}

Task<void> ImageData::ensureValid(string_view channelSelector, int taskPriority) {
    if (channels.empty()) {
        throw ImageLoadError{"Image must have at least one channel."};
    }

    if (orientation != EOrientation::TopLeft) {
        co_await orientToTopLeft(taskPriority);
    }

    // No data window? Default to the channel size
    if (!dataWindow.isValid()) {
        dataWindow = channels.front().size();
    }

    if (!displayWindow.isValid()) {
        displayWindow = channels.front().size();
    }

    for (auto& c : channels) {
        if (c.size() != size()) {
            throw ImageLoadError{
                fmt::format("All channels must have the same size as the data window. ({}: {} != {})", c.name(), c.size(), size())
            };
        }

        if (!c.name().starts_with(partName)) {
            c.setName(Channel::joinIfNonempty(partName, c.name()));
        }
    }

    if (!channelSelector.empty()) {
        vector<pair<size_t, size_t>> matches;
        for (size_t i = 0; i < channels.size(); ++i) {
            size_t matchId;
            if (matchesFuzzy(channels[i].name(), channelSelector, &matchId)) {
                matches.emplace_back(matchId, i);
            }
        }

        sort(begin(matches), end(matches));

        // Prune and sort channels by the channel selector
        vector<Channel> tmp = std::move(channels);
        channels.clear();

        for (const auto& match : matches) {
            channels.emplace_back(std::move(tmp[match.second]));
        }
    }

    if (layers.empty()) {
        set<string> layerNames;
        for (auto& c : channels) {
            layerNames.emplace(Channel::head(c.name()));
        }

        for (string_view l : layerNames) {
            layers.emplace_back(l);
        }
    }

    if (!hasPremultipliedAlpha) {
        co_await multiplyAlpha(taskPriority);
    }

    co_await convertToRec709(taskPriority);

    // NOTE: Lossy compression seems to ruin reliable derivations of the white level from maxCLL values. maxFALL values should work in
    // principle, but the only dataset I have with those is https://people.csail.mit.edu/ericchan/hdr/ where the maxFALL values seem to be
    // incorrect. (Do not match what the PQ transfer prescribes by inconsistent amounts. I might be doing something wrong.)

    // co_await deriveWhiteLevelFromMetadata(taskPriority);

    co_await convertToDesiredPixelFormat(taskPriority);

    TEV_ASSERT(hasPremultipliedAlpha, "tev assumes an internal pre-multiplied-alpha representation.");
    TEV_ASSERT(toRec709 == Matrix3f{1.0f}, "tev assumes an images to be internally represented in sRGB/Rec709 space.");

    attributes.emplace_back(hdrMetadata.toAttributes());
}

atomic<int> Image::sId(0);

Image::Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, string_view channelSelector, bool groupChannels) :
    mPath{path}, mFileLastModified{fileLastModified}, mChannelSelector{channelSelector}, mData{std::move(data)}, mId{Image::drawId()} {
    mName = channelSelector.empty() ? tev::toDisplayString(path) : fmt::format("{}:{}", tev::toDisplayString(path), channelSelector);

    if (groupChannels) {
        for (const auto& l : mData.layers) {
            const auto groups = getGroupedChannels(l);
            mChannelGroups.insert(end(mChannelGroups), begin(groups), end(groups));
        }
    } else {
        // If we don't group channels, then each channel is its own group.
        for (const auto& c : mData.channels) {
            mChannelGroups.emplace_back(
                ChannelGroup{
                    string{c.name()},
                    vector<string>{string{c.name()}, string{c.name()}, string{c.name()}}
            }
            );
        }
    }

    // Ensure that alpha channels are last in their group
    for (const auto& group : mChannelGroups) {
        for (const auto& channel : group.channels) {
            TEV_ASSERT(Channel::tail(channel) != "A" || &channel == &group.channels.back(), "Alpha channel must be last in channel group.");
        }
    }
}

Image::~Image() {
    // Move the texture pointers to the main thread such that their reference count hits zero there. This is required, because OpenGL calls
    // must always happen on the main thread.
    scheduleToMainThread([textures = std::move(mTextures)] {});

    if (mStaleIdCallback) {
        mStaleIdCallback(mId);
    }
}

string Image::shortName() const {
    string result = mName;

    size_t slashPosition = result.find_last_of("/\\");
    if (slashPosition != string::npos) {
        result = result.substr(slashPosition + 1);
    }

    size_t colonPosition = result.find_last_of(":");
    if (colonPosition != string::npos) {
        result = result.substr(0, colonPosition);
    }

    return result;
}

bool Image::isInterleaved(span<const string> channelNames, size_t desiredBytesPerSample, size_t desiredStride) const {
    if (desiredStride < channelNames.size()) {
        throw runtime_error{"Desired stride must be at least the number of channels."};
    }

    // It's fine if there are fewer than 4 channels -- they may still have been allocated as part of an interleaved RGBA buffer where some
    // of these 4 channels have default values. The following loop checks that the stride is 4 and that all present channels are adjacent.
    const uint8_t* interleavedData = nullptr;
    for (size_t i = 0; i < channelNames.size(); ++i) {
        const auto* chan = channel(channelNames[i]);
        if (!chan) {
            return false;
        }

        if (i == 0) {
            interleavedData = chan->data();
        }

        if (interleavedData != chan->data() - i * desiredBytesPerSample || chan->stride() != desiredStride) {
            return false;
        }
    }

    return interleavedData;
}

static size_t numChannelsInPixelFormat(Texture::PixelFormat pixelFormat) {
    switch (pixelFormat) {
        case Texture::PixelFormat::R: return 1;
        case Texture::PixelFormat::RA: return 2;
        case Texture::PixelFormat::RGB: return 3;
        case Texture::PixelFormat::RGBA: return 4;
        default: throw runtime_error{"Unsupported pixel format for texture."};
    }
}

static size_t bitsPerSampleInComponentFormat(Texture::ComponentFormat componentFormat) {
    switch (componentFormat) {
        case Texture::ComponentFormat::Float16: return 16;
        case Texture::ComponentFormat::Float32: return 32;
        default: throw runtime_error{"Unsupported component format for texture."};
    }
}

template <typename T>
Task<void> prepareTextureChannel(
    T* data, const Channel* chan, const Vector2i& pos, const Vector2i& size, size_t channelIdx, size_t numTextureChannels
) {
    const bool isAlpha = channelIdx == 3 || (chan && Channel::isAlpha(chan->name()));
    const T defaultVal = isAlpha ? (T)1.0f : (T)0.0f;
    const size_t numPixels = (size_t)size.x() * size.y();

    if (chan) {
        auto copyChannel = [&](const auto* src) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
                numPixels,
                [chan, src, &data, numTextureChannels, channelIdx, width = size.x(), pos](int y) {
                    for (int x = 0; x < width; ++x) {
                        size_t tileIdx = x + y * (size_t)width;
                        data[tileIdx * numTextureChannels + channelIdx] = chan->typedDataAt(src, {pos.x() + x, pos.y() + y});
                    }
                },
                numeric_limits<int>::max()
            );
        };

        switch (chan->pixelFormat()) {
            case EPixelFormat::U8: co_await copyChannel(chan->data()); break;
            case EPixelFormat::U16: co_await copyChannel((const uint16_t*)chan->data()); break;
            case EPixelFormat::F16: co_await copyChannel((const half*)chan->data()); break;
            case EPixelFormat::F32: co_await copyChannel((const float*)chan->data()); break;
        }
    } else {
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            numPixels,
            numPixels,
            [&data, defaultVal, numTextureChannels, channelIdx](size_t j) { data[j * numTextureChannels + channelIdx] = defaultVal; },
            numeric_limits<int>::max()
        );
    }
}

Texture* Image::texture(span<const string> channelNames, EInterpolationMode minFilter, EInterpolationMode magFilter) {
    if (size().x() > maxTextureSize() || size().y() > maxTextureSize()) {
        tlog::error() << fmt::format("{} is too large for Texturing. ({}x{})", mName, size().x(), size().y());
        return nullptr;
    }

    const string lookup = fmt::format("{}-{}-{}", join(channelNames, ","), tev::toString(minFilter), tev::toString(magFilter));
    const auto iter = mTextures.find(lookup);
    if (iter != end(mTextures)) {
        auto& texture = iter->second;
        if (texture.mipmapDirty && minFilter == EInterpolationMode::Trilinear) {
            texture.nanoguiTexture->generate_mipmap();
            texture.mipmapDirty = false;
        }

        return texture.nanoguiTexture.get();
    }

    const auto toNanogui = [](EInterpolationMode mode) {
        switch (mode) {
            case EInterpolationMode::Nearest: return Texture::InterpolationMode::Nearest;
            case EInterpolationMode::Bilinear: return Texture::InterpolationMode::Bilinear;
            case EInterpolationMode::Trilinear: return Texture::InterpolationMode::Trilinear;
            default: throw runtime_error{"Unknown interpolation mode."};
        }
    };

    Texture::PixelFormat pixelFormat;
    switch (channelNames.size()) {
        case 1: pixelFormat = Texture::PixelFormat::R; break;
        case 2: pixelFormat = Texture::PixelFormat::RA; break;
        case 3: pixelFormat = Texture::PixelFormat::RGB; break;
        // case 3: pixelFormat = Texture::PixelFormat::RGBA; break; // Uncomment for faster GPU uploads at the cost of memory
        case 4: pixelFormat = Texture::PixelFormat::RGBA; break;
        default: throw runtime_error{"Unsupported number of channels for texture."};
    }

    Texture::ComponentFormat componentFormat = Texture::ComponentFormat::Float16;
    for (const auto& chanName : channelNames) {
        const Channel* chan = channel(chanName);
        if (chan && chan->desiredPixelFormat() == EPixelFormat::F32) {
            componentFormat = Texture::ComponentFormat::Float32;
            break; // No need to check further, we already have a channel that requires F32.
        }
    }

    mTextures.emplace(
        lookup,
        ImageTexture{
            new Texture{
                        pixelFormat, componentFormat,
                        {size().x(), size().y()},
                        toNanogui(minFilter),
                        toNanogui(magFilter),
                        Texture::WrapMode::ClampToEdge,
                        1, Texture::TextureFlags::ShaderRead,
                        true, },
            {channelNames.begin(), channelNames.end()},
            false,
    }
    );

    auto& texture = mTextures.at(lookup).nanoguiTexture;

    const size_t numTextureChannels = numChannelsInPixelFormat(texture->pixel_format());
    const size_t bitsPerSample = bitsPerSampleInComponentFormat(texture->component_format());

    // Important: num channels can be *larger* than the number of channels in the image here!
    // This is because some graphics APIs, like metal, only have power-of-two channel counts: 1, 2, 4
    if (numTextureChannels < channelNames.size()) {
        throw runtime_error{fmt::format(
            "Image has {} channels, but texture requires at least {} channels. (Image: {}, Texture: {})",
            channelNames.size(),
            numTextureChannels,
            mName,
            lookup
        )};
    }

    const size_t bytesPerSample = bitsPerSample / 8;
    const bool directUpload = isInterleaved(channelNames, bytesPerSample, numTextureChannels * bytesPerSample);

    tlog::debug() << fmt::format(
        "Uploading texture: direct={} bps={} filter={}-{} img={}:{}",
        directUpload,
        bitsPerSample,
        tev::toString(minFilter),
        tev::toString(magFilter),
        mName,
        join(channelNames, ",")
    );

    using DataBufPtr = std::shared_ptr<Channel::Data>;
    DataBufPtr dataPtr = nullptr;

    // Check if channel layout is already interleaved. If yes, can directly copy onto GPU!
    if (directUpload) {
        const Channel* chan = channel(channelNames[0]);
        dataPtr = chan->dataBuf();
    } else {
        ScopeGuard guard{[now = chrono::system_clock::now()]() {
            const auto duration = chrono::duration_cast<chrono::duration<double>>(chrono::system_clock::now() - now);
            tlog::debug() << fmt::format("Upload buffer generation took {:.03}s", duration.count());
        }};

        const auto numPixels = this->numPixels();
        const auto size = this->size();
        dataPtr = make_shared<Channel::Data>(numPixels * numTextureChannels * (bitsPerSample / 8));

        vector<Task<void>> tasks;
        for (size_t i = 0; i < numTextureChannels; ++i) {
            const Channel* chan = i < channelNames.size() ? channel(channelNames[i]) : nullptr;
            switch (texture->component_format()) {
                case Texture::ComponentFormat::Float16:
                    tasks.emplace_back(prepareTextureChannel((half*)dataPtr->data(), chan, {0, 0}, size, i, numTextureChannels));
                    break;
                case Texture::ComponentFormat::Float32:
                    tasks.emplace_back(prepareTextureChannel((float*)dataPtr->data(), chan, {0, 0}, size, i, numTextureChannels));
                    break;
                default: throw runtime_error{"Unsupported component format for texture."};
            }
        }

        waitAll(tasks);
    }

    // If the backend supports it, schedule an async copy that uses DMA to
    // copy the texture without blocking the host. The operation is part of
    // the graphics queue and correctly ordered wrt. other display
    // operations. On Apple M* GPUs, CPU/GPU share the same memory, so this
    // step just converts the texture into a more suitable layout.

    texture->upload_async(dataPtr->data(), [](void* p) { delete (DataBufPtr*)p; }, new DataBufPtr(dataPtr));

    if (minFilter == EInterpolationMode::Trilinear) {
        texture->generate_mipmap();
    }

    return texture.get();
}

vector<string> Image::channelsInGroup(string_view groupName) const {
    for (const auto& group : mChannelGroups) {
        if (group.name == groupName) {
            return group.channels;
        }
    }

    return {};
}

void Image::decomposeChannelGroup(string_view groupName) {
    // Takes all channels of a given group and turns them into individual groups.

    auto group = find_if(mChannelGroups.begin(), mChannelGroups.end(), [&](const auto& g) { return g.name == groupName; });
    if (group == mChannelGroups.end()) {
        return;
    }

    const auto& channels = group->channels;
    if (channels.empty()) {
        return;
    }

    auto groupPos = distance(mChannelGroups.begin(), group);
    for (const auto& channel : channels) {
        mChannelGroups.insert(mChannelGroups.begin() + (++groupPos), ChannelGroup{channel, {channel}});
    }

    // Duplicates may have appeared here. (E.g. when trying to decompose a single channel or when single-color channels appear multiple
    // times in their group to render as RGB rather than pure red.) Don't insert those.
    removeDuplicates(mChannelGroups);
}

vector<ChannelGroup> Image::getGroupedChannels(string_view layerName) const {
    vector<vector<string>> groups = {
        {"R", "G", "B"},
        {"r", "g", "b"},
        {"X", "Y", "Z"},
        {"x", "y", "z"},
        {"U", "V"},
        {"u", "v"},
        {"Z"},
        {"z"},
    };

    auto createChannelGroup = [](string_view layer, vector<string> channels) {
        TEV_ASSERT(!channels.empty(), "Can't create a channel group without channels.");

        vector<string_view> channelTails = {channels.begin(), channels.end()};
        channelTails.erase(unique(begin(channelTails), end(channelTails)), end(channelTails));
        transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

        const string channelsString = join(channelTails, ",");
        const string name = layer.empty() ?
            channelsString :
            (channelTails.size() == 1 ? fmt::format("{}{}", layer, channelsString) : fmt::format("{}({})", layer, channelsString));

        return ChannelGroup{name, std::move(channels)};
    };

    string alphaChannelName = fmt::format("{}A", layerName);

    vector<string> allChannels = mData.channelsInLayer(layerName);

    auto alphaIt = find(begin(allChannels), end(allChannels), alphaChannelName);
    bool hasAlpha = alphaIt != end(allChannels);
    if (hasAlpha) {
        allChannels.erase(alphaIt);
    }

    vector<ChannelGroup> result;

    for (const auto& group : groups) {
        vector<string> groupChannels;
        for (string_view channel : group) {
            string name = fmt::format("{}{}", layerName, channel);
            auto it = find(begin(allChannels), end(allChannels), name);
            if (it != end(allChannels)) {
                groupChannels.emplace_back(name);
                allChannels.erase(it);
            }
        }

        if (!groupChannels.empty()) {
            if (hasAlpha) {
                groupChannels.emplace_back(alphaChannelName);
            }

            result.emplace_back(createChannelGroup(layerName, std::move(groupChannels)));
        }
    }

    for (const auto& name : allChannels) {
        if (hasAlpha) {
            result.emplace_back(createChannelGroup(layerName, vector<string>{name, alphaChannelName}));
        } else {
            result.emplace_back(createChannelGroup(layerName, vector<string>{name}));
        }
    }

    if (hasAlpha && result.empty()) {
        result.emplace_back(createChannelGroup(layerName, vector<string>{alphaChannelName}));
    }

    TEV_ASSERT(!result.empty(), "Images with no channels should never exist.");

    return result;
}

vector<string> Image::getExistingChannels(span<const string> requestedChannels) const {
    vector<string> result;
    copy_if(begin(requestedChannels), end(requestedChannels), back_inserter(result), [&](string_view c) { return hasChannel(c); });
    return result;
}

void Image::updateChannel(string_view channelName, int x, int y, int width, int height, span<const float> data) {
    Channel* const chan = mutableChannel(channelName);
    if (!chan) {
        tlog::warning() << "Channel " << channelName << " could not be updated, because it does not exist.";
        return;
    }

    chan->updateTile(x, y, width, height, data);

    // Update textures that are cached for this channel
    for (auto&& kv : mTextures) {
        auto& imageTexture = kv.second;
        if (find(begin(imageTexture.channels), end(imageTexture.channels), channelName) == end(imageTexture.channels)) {
            continue;
        }

        const auto numPixels = (size_t)width * height;
        const size_t numTextureChannels = numChannelsInPixelFormat(imageTexture.nanoguiTexture->pixel_format());
        const size_t bitsPerSample = bitsPerSampleInComponentFormat(imageTexture.nanoguiTexture->component_format());
        HeapArray<uint8_t> textureData(numPixels * numTextureChannels * (bitsPerSample / 8));

        vector<Task<void>> tasks;
        for (size_t i = 0; i < numTextureChannels; ++i) {
            const Channel* chan = i < imageTexture.channels.size() ? channel(imageTexture.channels[i]) : nullptr;
            switch (imageTexture.nanoguiTexture->component_format()) {
                case Texture::ComponentFormat::Float16:
                    tasks.emplace_back(prepareTextureChannel((half*)textureData.data(), chan, {x, y}, {width, height}, i, numTextureChannels));
                    break;
                case Texture::ComponentFormat::Float32:
                    tasks.emplace_back(prepareTextureChannel((float*)textureData.data(), chan, {x, y}, {width, height}, i, numTextureChannels));
                    break;
                default: throw runtime_error{"Unsupported component format for texture."};
            }
        }

        waitAll(tasks);
        imageTexture.nanoguiTexture->upload_sub_region(textureData.data(), {x, y}, {width, height});
        imageTexture.mipmapDirty = true;
    }
}

void Image::updateVectorGraphics(bool append, span<const VgCommand> commands) {
    if (!append) {
        mVgCommands.clear();
    }

    copy(begin(commands), end(commands), back_inserter(mVgCommands));
}

Task<vector<Channel>> Image::getHdrImageData(shared_ptr<Image> reference, string_view requestedChannelGroup, EMetric metric, int priority) const {
    const auto size = this->size();
    const auto numPixels = this->numPixels();

    vector<Channel> result;
    const auto channelNames = channelsInGroup(requestedChannelGroup);
    for (size_t i = 0; i < channelNames.size(); ++i) {
        result.emplace_back(toUpper(Channel::tail(channelNames[i])), size, EPixelFormat::F32, EPixelFormat::F32);
    }

    const auto channels = this->channels(channelNames);
    if (!reference) {
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * channels.size(),
            [&](size_t j) {
                for (size_t c = 0; c < channels.size(); ++c) {
                    result[c].setAt(j, channels[c]->at(j));
                }
            },
            priority
        );
    } else {
        const auto referenceChannels = reference->channels(channelNames);
        const auto offset = (reference->size() - size) / 2;

        vector<bool> isAlpha(channelNames.size());
        for (size_t i = 0; i < channelNames.size(); ++i) {
            isAlpha[i] = Channel::isAlpha(channelNames[i]);
        }

        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            numPixels * channels.size(),
            [&](int y) {
                for (size_t c = 0; c < channels.size(); ++c) {
                    const auto* channel = channels[c];
                    const auto* referenceChannel = referenceChannels[c];

                    if (isAlpha[c]) {
                        for (int x = 0; x < size.x(); ++x) {
                            result[c].setAt(
                                {x, y},
                                0.5f *
                                    (channel->eval({x, y}) +
                                     (referenceChannel ? referenceChannel->eval({x + offset.x(), y + offset.y()}) : 1.0f))
                            );
                        }
                    } else {
                        for (int x = 0; x < size.x(); ++x) {
                            result[c].setAt(
                                {x, y},
                                applyMetric(
                                    channel->eval({x, y}), referenceChannel ? referenceChannel->eval({x + offset.x(), y + offset.y()}) : 0.0f, metric
                                )
                            );
                        }
                    }
                }
            },
            priority
        );
    }

    co_return result;
}

Task<HeapArray<float>> Image::getRgbaHdrImageData(
    shared_ptr<Image> reference, const Box2i& imageRegion, string_view requestedChannelGroup, EMetric metric, bool divideAlpha, int priority
) const {
    const auto& channels = co_await getHdrImageData(reference, requestedChannelGroup, metric, priority);
    if (channels.empty()) {
        co_return {};
    }

    const auto numPixels = (size_t)imageRegion.area();
    const auto nChannelsToSave = std::min((int)channels.size(), 4);

    // Flatten image into vector
    HeapArray<float> result{4 * numPixels};

    co_await ThreadPool::global().parallelForAsync(
        imageRegion.min.y(),
        imageRegion.max.y(),
        numPixels * 4,
        [nChannelsToSave, &channels, &result, &imageRegion](int y) {
            int yresult = y - imageRegion.min.y();
            for (int x = imageRegion.min.x(); x < imageRegion.max.x(); ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float val = c < nChannelsToSave ? channels[c].eval({x, y}) : c == 3 ? 1.0f : 0.0f;
                    const int xresult = x - imageRegion.min.x();
                    result[(yresult * imageRegion.size().x() + xresult) * 4 + c] = val;
                }
            }
        },
        priority
    );

    // Divide alpha out if needed (for storing in non-premultiplied formats)
    if (divideAlpha) {
        co_await ThreadPool::global().parallelForAsync(
            (size_t)0,
            numPixels,
            numPixels * 4,
            [&result](size_t j) {
                const float alpha = result[j * 4 + 3];
                const float factor = alpha == 0 ? 0 : 1 / alpha;
                for (int c = 0; c < 3; ++c) {
                    result[j * 4 + c] *= factor;
                }
            },
            priority
        );
    }

    co_return result;
}

Task<HeapArray<uint8_t>> Image::getRgbaLdrImageData(
    const HeapArray<float>& rgbaHdrData, ETonemap tonemap, float gamma, float exposure, float offset, int priority
) const {
    HeapArray<uint8_t> result(rgbaHdrData.size());

    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        rgbaHdrData.size() / 4,
        rgbaHdrData.size(),
        [&](const size_t i) {
            const size_t start = 4 * i;
            const Vector3f rgb = applyTonemap(
                {
                    applyExposureAndOffset(rgbaHdrData[start + 0], exposure, offset),
                    applyExposureAndOffset(rgbaHdrData[start + 1], exposure, offset),
                    applyExposureAndOffset(rgbaHdrData[start + 2], exposure, offset),
                },
                gamma,
                tonemap
            );

            const auto rgba = Vector4f{rgb.x(), rgb.y(), rgb.z(), rgbaHdrData[start + 3]};

            for (int j = 0; j < 4; ++j) {
                result[start + j] = (uint8_t)(clamp(rgba[j], 0.0f, 1.0f) * 255 + 0.5f);
            }
        },
        priority
    );

    co_return result;
}

Task<HeapArray<uint8_t>> Image::getRgbaLdrImageData(
    shared_ptr<Image> reference,
    const Box2i& imageRegion,
    string_view requestedChannelGroup,
    EMetric metric,
    bool divideAlpha,
    ETonemap tonemap,
    float gamma,
    float exposure,
    float offset,
    int priority
) const {
    co_return co_await getRgbaLdrImageData(
        co_await getRgbaHdrImageData(reference, imageRegion, requestedChannelGroup, metric, divideAlpha, priority),
        tonemap,
        gamma,
        exposure,
        offset,
        priority
    );
}

Task<void> Image::save(
    const fs::path& path,
    shared_ptr<Image> reference,
    const Box2i& imageRegion,
    string_view requestedChannelGroup,
    EMetric metric,
    ETonemap tonemap,
    float gamma,
    float exposure,
    float offset,
    int priority
) const {
    if (path.empty()) {
        throw ImageSaveError{"You must specify a file name to save the image."};
    }

    if (path.extension().empty()) {
        throw ImageSaveError{"You must specify a file extension or select one from the dropdown to save the image."};
    }

    const auto size = imageRegion.size();
    if (size.x() == 0 || size.y() == 0) {
        throw ImageSaveError{"Can not save image with zero pixels."};
    }

    ofstream f{path, ios_base::binary};
    if (!f) {
        throw ImageSaveError{fmt::format("Could not open file {}", path)};
    }

    for (const auto& saver : ImageSaver::getSavers()) {
        if (!saver->canSaveFile(path)) {
            continue;
        }

        const auto* hdrSaver = dynamic_cast<const TypedImageSaver<float>*>(saver.get());
        const auto* ldrSaver = dynamic_cast<const TypedImageSaver<uint8_t>*>(saver.get());

        const bool divideAlpha = saver->alphaKind(path) == EAlphaKind::Straight;
        const auto rgbaHdrData = co_await getRgbaHdrImageData(reference, imageRegion, requestedChannelGroup, metric, divideAlpha, priority);

        if (hdrSaver) {
            co_await hdrSaver->save(f, path, rgbaHdrData, size, 4);
        } else if (ldrSaver) {
            const auto rgbaLdrData = co_await getRgbaLdrImageData(rgbaHdrData, tonemap, gamma, exposure, offset, priority);
            co_await ldrSaver->save(f, path, rgbaLdrData, size, 4);
        } else {
            TEV_ASSERT(false, "Each image saver must either be a HDR or an LDR saver.");
        }

        co_return;
    }

    throw ImageSaveError{fmt::format("No save routine for image type {} found.", path.extension())};
}

template <typename T> time_t to_time_t(T timePoint) {
    // `clock_cast` appears to throw errors on some systems, so we're using this slightly hacky inaccurate/random time conversion (now() is
    // not called simultaneously for both clocks) in order to convert to system time.
    using namespace chrono;
    return system_clock::to_time_t(time_point_cast<system_clock::duration>(timePoint - T::clock::now() + system_clock::now()));
}

string Image::toString() const {
    stringstream sstream;
    sstream << mName << "\n\n";

    {
        const time_t cftime = to_time_t(mFileLastModified);
        sstream << "Last modified:\n" << asctime(localtime(&cftime)) << "\n";
    }

    sstream << "Resolution: (" << size().x() << ", " << size().y() << ")\n";
    if (displayWindow() != dataWindow() || displayWindow().min != Vector2i{0}) {
        sstream << "Display window: (" << displayWindow().min.x() << ", " << displayWindow().min.y() << ")(" << displayWindow().max.x()
                << ", " << displayWindow().max.y() << ")\n";
        sstream << "Data window: (" << dataWindow().min.x() << ", " << dataWindow().min.y() << ")(" << dataWindow().max.x() << ", "
                << dataWindow().max.y() << ")\n";
    }

    sstream << "\nChannels:\n";

    auto localLayers = mData.layers;
    transform(begin(localLayers), end(localLayers), begin(localLayers), [this](string layer) {
        auto channels = mData.channelsInLayer(layer);
        transform(begin(channels), end(channels), begin(channels), [](string channel) { return Channel::tail(channel); });

        if (layer.empty()) {
            return join(channels, ",");
        } else if (channels.size() == 1) {
            return layer + channels.front();
        } else {
            return layer + "("s + join(channels, ",") + ")"s;
        }
    });

    sstream << join(localLayers, "\n");
    return sstream.str();
}

// Modifies `data` and returns the new size of the data after reorientation.
Task<nanogui::Vector2i>
    orientToTopLeft(EPixelFormat format, Channel::Data& data, nanogui::Vector2i size, EOrientation orientation, int priority) {
    if (orientation == EOrientation::TopLeft) {
        co_return size;
    }

    bool swapAxes = orientation >= EOrientation::LeftTop;
    size = swapAxes ? nanogui::Vector2i{size.y(), size.x()} : size;
    nanogui::Vector2i otherSize = swapAxes ? nanogui::Vector2i{size.y(), size.x()} : size;

    const size_t numBytesPerSample = nBytes(format);
    const size_t numPixels = (size_t)size.x() * size.y();

    if (numPixels == 0) {
        co_return size;
    } else if (data.size() % (numPixels * numBytesPerSample) != 0) {
        throw ImageModifyError{"Image data size is not a multiple of the number of pixels."};
    }

    const size_t numSamplesPerPixel = data.size() / numPixels / numBytesPerSample;
    const size_t numBytesPerPixel = numSamplesPerPixel * numBytesPerSample;

    Channel::Data reorientedData(data.size());
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        numPixels,
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                const size_t i = y * (size_t)size.x() + x;

                const auto other = applyOrientation(orientation, nanogui::Vector2i{x, y}, size);
                const size_t j = other.y() * (size_t)otherSize.x() + other.x();

                std::memcpy(&reorientedData[i * numBytesPerPixel], &data[j * numBytesPerPixel], numBytesPerPixel);
            }
        },
        priority
    );

    std::swap(data, reorientedData);
    co_return size;
}

Task<vector<shared_ptr<Image>>>
    tryLoadImage(int taskPriority, fs::path path, istream& iStream, string_view channelSelector, bool applyGainmaps, bool groupChannels) {
    const auto handleException = [&](const exception& e) {
        if (channelSelector.empty()) {
            tlog::error() << fmt::format("Could not load {}: {}", toString(path), e.what());
        } else {
            tlog::error() << fmt::format("Could not load {}:{}: {}", toString(path), channelSelector, e.what());
        }
    };

    // No need to keep loading images if tev is already shutting down again.
    if (shuttingDown()) {
        co_return {};
    }

    try {
        const auto start = chrono::system_clock::now();

        if (!iStream) {
            throw ImageLoadError{fmt::format("Image {} could not be opened.", path)};
        }

        fs::file_time_type fileLastModified = fs::file_time_type::clock::now();
        if (fs::exists(path)) {
            // Unlikely, but the file could have been deleted, moved, or something else might have happened to it that makes obtaining its
            // last modified time impossible. Ignore such errors.
            try {
                fileLastModified = fs::last_write_time(path);
            } catch (...) {}
        }

        string loadMethod;
        vector<ImageData> imageData;
        bool success = false;
        for (const auto& imageLoader : ImageLoader::getLoaders()) {
            try {
                loadMethod = imageLoader->name();
                imageData = co_await imageLoader->load(iStream, path, channelSelector, taskPriority, applyGainmaps);
                success = true;
                break;
            } catch (const ImageLoader::FormatNotSupported& e) {
                tlog::debug()
                    << fmt::format("Image loader {} does not support loading {}: {} Trying next loader.", loadMethod, path, e.what());

                // Reset file cursor to beginning and try next loader.
                iStream.clear();
                iStream.seekg(0);
            }
        }

        if (!success) {
            throw ImageLoadError{"No suitable image loader found."};
        }

        vector<shared_ptr<Image>> images;
        for (auto& i : imageData) {
            co_await i.ensureValid(channelSelector, taskPriority);

            if (i.channels.empty()) {
                continue;
            }

            // If multiple image "parts" were loaded and they have names, ensure that these names are present in the channel selector.
            string localChannelSelector;
            if (i.partName.empty()) {
                localChannelSelector = string{channelSelector};
            } else {
                const auto selectorParts = split(channelSelector, ",");
                if (channelSelector.empty()) {
                    localChannelSelector = i.partName;
                } else if (find(begin(selectorParts), end(selectorParts), i.partName) == end(selectorParts)) {
                    localChannelSelector = fmt::format("{},{}", i.partName, channelSelector);
                } else {
                    localChannelSelector = string{channelSelector};
                }
            }

            images.emplace_back(make_shared<Image>(path, fileLastModified, std::move(i), localChannelSelector, groupChannels));
        }

        if (images.empty()) {
            throw ImageLoadError{fmt::format("No parts/channels match channel selector :{}", channelSelector)};
        }

        const auto end = chrono::system_clock::now();
        const chrono::duration<double> elapsedSeconds = end - start;

        tlog::success() << fmt::format("Loaded {} via {} after {:.3f} seconds.", toString(path), loadMethod, elapsedSeconds.count());

        co_return images;
    } catch (const ImageLoadError& e) { handleException(e); } catch (const ImageModifyError& e) {
        handleException(e);
    }

    co_return {};
}

Task<vector<shared_ptr<Image>>>
    tryLoadImage(fs::path path, istream& iStream, string_view channelSelector, bool applyGainmaps, bool groupChannels) {
    co_return co_await tryLoadImage(-Image::drawId(), path, iStream, channelSelector, applyGainmaps, groupChannels);
}

Task<vector<shared_ptr<Image>>>
    tryLoadImage(int taskPriority, fs::path path, string_view channelSelector, bool applyGainmaps, bool groupChannels) {
    try {
        path = fs::absolute(path);
    } catch (const runtime_error&) {
        // If for some strange reason we can not obtain an absolute path, let's still try to open the image at the given path just to make
        // sure.
    }

    ifstream fileStream{path, ios_base::binary};
    co_return co_await tryLoadImage(taskPriority, path, fileStream, channelSelector, applyGainmaps, groupChannels);
}

Task<vector<shared_ptr<Image>>> tryLoadImage(fs::path path, string_view channelSelector, bool applyGainmaps, bool groupChannels) {
    co_return co_await tryLoadImage(-Image::drawId(), path, channelSelector, applyGainmaps, groupChannels);
}

void BackgroundImagesLoader::enqueue(const fs::path& path, string_view channelSelector, bool shallSelect, const shared_ptr<Image>& toReplace) {
    // If we're trying to open a directory, try loading all the images inside of that directory
    if (fs::exists(path) && fs::is_directory(path)) {
        tlog::info() << "Loading images " << (mRecursiveDirectories ? "recursively " : "") << "from directory " << toString(path);

        const fs::path canonicalPath = fs::canonical(path);
        mDirectories[canonicalPath].emplace(channelSelector);

        vector<fs::directory_entry> entries;
        forEachFileInDir(mRecursiveDirectories, canonicalPath, [&](const auto& entry) {
            if (!entry.is_directory()) {
                mFilesFoundInDirectories.emplace(PathAndChannelSelector{entry, string{channelSelector}});
                entries.emplace_back(entry);
            }
        });

        // Open directory entries in natural order (e.g. "file1.exr", "file2.exr", "file10.exr" instead of "file1.exr", "file10.exr"),
        // selecting the first one.
        sort(begin(entries), end(entries), [](const auto& a, const auto& b) { return naturalCompare(a.path().string(), b.path().string()); });

        for (size_t i = 0; i < entries.size(); ++i) {
            enqueue(entries[i], channelSelector, i == 0 ? shallSelect : false);
        }

        return;
    }

    // We want to measure the time it takes to load a whole batch of images. Start measuring when the loader queue goes from empty to
    // non-empty and stop measuring when the queue goes from non-empty to empty again.
    if (mUnsortedLoadCounter == mLoadCounter) {
        mLoadStartTime = chrono::system_clock::now();
        mLoadStartCounter = mUnsortedLoadCounter;
    }

    const int loadId = mUnsortedLoadCounter++;
    invokeTaskDetached([loadId, path, channelSelector = string{channelSelector}, shallSelect, toReplace, this]() -> Task<void> {
        const int taskPriority = -Image::drawId();

        co_await ThreadPool::global().enqueueCoroutine(taskPriority);
        const auto images = co_await tryLoadImage(taskPriority, path, channelSelector, mApplyGainmaps, mGroupChannels);

        {
            const lock_guard lock{mPendingLoadedImagesMutex};
            mPendingLoadedImages.push({loadId, shallSelect, images, toReplace});
        }

        if (publishSortedLoads()) {
            redrawWindow();
        }
    });
}

void BackgroundImagesLoader::checkDirectoriesForNewFilesAndLoadThose() {
    for (const auto& dir : mDirectories) {
        forEachFileInDir(mRecursiveDirectories, dir.first, [&](const auto& entry) {
            if (!entry.is_directory()) {
                for (const auto& channelSelector : dir.second) {
                    const PathAndChannelSelector p = {entry, channelSelector};
                    if (!mFilesFoundInDirectories.contains(p)) {
                        mFilesFoundInDirectories.emplace(p);
                        enqueue(entry, channelSelector, false);
                    }
                }
            }
        });
    }
}

optional<ImageAddition> BackgroundImagesLoader::tryPop() {
    const lock_guard lock{mPendingLoadedImagesMutex};
    return mLoadedImages.tryPop();
}

optional<nanogui::Vector2i> BackgroundImagesLoader::firstImageSize() const {
    const lock_guard lock{mPendingLoadedImagesMutex};
    if (mLoadedImages.empty()) {
        return nullopt;
    }

    const ImageAddition& firstImage = mLoadedImages.front();
    if (firstImage.images.empty()) {
        return nullopt;
    }

    return firstImage.images.front()->size();
}

bool BackgroundImagesLoader::publishSortedLoads() {
    const lock_guard lock{mPendingLoadedImagesMutex};
    bool pushed = false;
    while (!mPendingLoadedImages.empty() && mPendingLoadedImages.top().loadId == mLoadCounter) {
        // null image pointers indicate failed loads. These shouldn't be pushed.
        if (!mPendingLoadedImages.top().images.empty()) {
            mLoadedImages.push(mPendingLoadedImages.top());
        }

        mPendingLoadedImages.pop();
        pushed = true;

        ++mLoadCounter;
    }

    if (mLoadCounter == mUnsortedLoadCounter && mLoadCounter - mLoadStartCounter > 1) {
        const auto end = chrono::system_clock::now();
        const chrono::duration<double> elapsedSeconds = end - mLoadStartTime;
        tlog::success() << fmt::format("Loaded {} images in {:.3f} seconds.", mLoadCounter - mLoadStartCounter, elapsedSeconds.count());
    }

    return pushed;
}

bool BackgroundImagesLoader::hasPendingLoads() const {
    const lock_guard lock{mPendingLoadedImagesMutex};
    return mLoadCounter != mUnsortedLoadCounter;
}

} // namespace tev
