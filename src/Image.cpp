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
#include <tev/imageio/ImageLoader.h>

#include <half.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <fstream>
#include <istream>
#include <map>
#include <unordered_set>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

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

    for (auto& task : tasks) {
        co_await task;
    }

    // Since the image data is now in Rec709 space, converting to Rec709 is the identity transform.
    toRec709 = Matrix3f{1.0f};
}

Task<void> ImageData::convertToDesiredPixelFormat(int priority) {
    // All channels sharing the same data buffer must be converted together to avoid multiple conversions of the same data.
    multimap<shared_ptr<vector<uint8_t>>, Channel*> channelsByData;
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

        shared_ptr<vector<uint8_t>> data = it->first;

        const size_t nSamples = data->size() / nBytes(sourceFormat);
        vector<uint8_t> convertedData(nSamples * nBytes(targetFormat));

        const uint8_t* const src = data->data();
        uint8_t* const dst = convertedData.data();

        const auto typedConvert = [nSamples, priority](const auto* typedSrc, auto* typedDst) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                nSamples,
                [typedSrc, typedDst](size_t i) { typedDst[i] = typedSrc[i]; },
                priority
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

        tlog::debug(
        ) << fmt::format("Converted {} channels from {} to {}.", distance(it, rangeEnd), toString(sourceFormat), toString(targetFormat));

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
    for (auto& task : tasks) {
        co_await task;
    }

    hasPremultipliedAlpha = true;
}

Task<void> ImageData::unmultiplyAlpha(int priority) {
    if (!hasPremultipliedAlpha) {
        throw ImageModifyError{"Can't divide by alpha twice."};
    }

    vector<Task<void>> tasks;
    alphaOperation([&](Channel& target, const Channel& alpha) { tasks.emplace_back(target.divideByAsync(alpha, priority)); });
    for (auto& task : tasks) {
        co_await task;
    }

    hasPremultipliedAlpha = false;
}

Task<void> ImageData::orientToTopLeft(int priority) {
    if (orientation == EOrientation::TopLeft) {
        co_return;
    }

    bool swapAxes = orientation >= EOrientation::LeftTop;

    struct DataDesc {
        shared_ptr<vector<uint8_t>> data;
        Vector2i size;

        struct Hash {
            size_t operator()(const DataDesc& interval) const { return hash<shared_ptr<vector<uint8_t>>>()(interval.data); }
        };

        bool operator==(const DataDesc& other) const { return data == other.data && size == other.size; }
    };

    unordered_set<DataDesc, DataDesc::Hash> channelData;
    for (auto& c : channels) {
        if (c.stride() != 1 && c.stride() != 4) {
            throw ImageModifyError{"ImageData::orientToTopLeft: only strides 1 and 4 are supported."};
        }

        channelData.insert({c.dataBuf(), c.size()});
        if (swapAxes) {
            c.setSize({c.size().y(), c.size().x()});
        }
    }

    for (auto& c : channelData) {
        co_await tev::orientToTopLeft(EPixelFormat::F32, *c.data, c.size, orientation, priority);
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

    for (const auto& c : channels) {
        if (c.size() != size()) {
            throw ImageLoadError{
                fmt::format("All channels must have the same size as the data window. ({}: {} != {})", c.name(), c.size(), size())
            };
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

        if (channels.empty()) {
            throw ImageLoadError{fmt::format("Channel selector :{} discards all channels.", channelSelector)};
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

    co_await convertToDesiredPixelFormat(taskPriority);

    TEV_ASSERT(hasPremultipliedAlpha, "tev assumes an internal pre-multiplied-alpha representation.");
    TEV_ASSERT(toRec709 == Matrix3f{1.0f}, "tev assumes an images to be internally represented in sRGB/Rec709 space.");
}

atomic<int> Image::sId(0);

Image::Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, string_view channelSelector, bool groupChannels) :
    mPath{path}, mFileLastModified{fileLastModified}, mChannelSelector{channelSelector}, mData{std::move(data)}, mId{Image::drawId()} {
    mName = channelSelector.empty() ? tev::toString(path) : fmt::format("{}:{}", tev::toString(path), channelSelector);

    if (groupChannels) {
        for (const auto& l : mData.layers) {
            auto groups = getGroupedChannels(l);
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

    if (chan) {
        auto copyChannel = [&](const auto* src) -> Task<void> {
            co_await ThreadPool::global().parallelForAsync<int>(
                0,
                size.y(),
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
        const size_t numPixels = (size_t)size.x() * size.y();
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
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

    // Check if channel layout is already interleaved. If yes, can directly copy onto GPU!
    if (directUpload) {
        ScopeGuard guard{[now = chrono::system_clock::now()]() {
            const auto duration = chrono::duration_cast<chrono::duration<double>>(chrono::system_clock::now() - now);
            tlog::debug() << fmt::format("Direct upload took {:.03}s", duration.count());
        }};

        texture->upload((uint8_t*)channel(channelNames[0])->data());
    } else {
        ScopeGuard guard{[now = chrono::system_clock::now()]() {
            const auto duration = chrono::duration_cast<chrono::duration<double>>(chrono::system_clock::now() - now);
            tlog::debug() << fmt::format("Indirect upload took {:.03}s", duration.count());
        }};

        const auto numPixels = this->numPixels();
        const auto size = this->size();
        vector<uint8_t> data(numPixels * numTextureChannels * (bitsPerSample / 8));

        vector<Task<void>> tasks;
        for (size_t i = 0; i < numTextureChannels; ++i) {
            const Channel* chan = i < channelNames.size() ? channel(channelNames[i]) : nullptr;
            switch (texture->component_format()) {
                case Texture::ComponentFormat::Float16:
                    tasks.emplace_back(prepareTextureChannel((half*)data.data(), chan, {0, 0}, size, i, numTextureChannels));
                    break;
                case Texture::ComponentFormat::Float32:
                    tasks.emplace_back(prepareTextureChannel((float*)data.data(), chan, {0, 0}, size, i, numTextureChannels));
                    break;
                default: throw runtime_error{"Unsupported component format for texture."};
            }
        }

        waitAll<Task<void>>(tasks);
        texture->upload(data.data());
    }

    if (minFilter == EInterpolationMode::Trilinear) {
        ScopeGuard guard{[now = chrono::system_clock::now()]() {
            const auto duration = chrono::duration_cast<chrono::duration<double>>(chrono::system_clock::now() - now);
            tlog::debug() << fmt::format("Mipmap generation took {:.03}s", duration.count());
        }};

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
        vector<uint8_t> textureData(numPixels * numTextureChannels * (bitsPerSample / 8));

        vector<Task<void>> tasks;
        for (size_t i = 0; i < numTextureChannels; ++i) {
            const Channel* chan = i < imageTexture.channels.size() ? channel(imageTexture.channels[i]) : nullptr;
            switch (imageTexture.nanoguiTexture->component_format()) {
                case Texture::ComponentFormat::Float16:
                    tasks.emplace_back(prepareTextureChannel((half*)textureData.data(), chan, {x, y}, {width, height}, i, numTextureChannels));
                    break;
                case Texture::ComponentFormat::Float32:
                    tasks.emplace_back(prepareTextureChannel((float*)textureData.data(), chan, {x, y}, {width, height}, i, numTextureChannels)
                    );
                    break;
                default: throw runtime_error{"Unsupported component format for texture."};
            }
        }

        waitAll<Task<void>>(tasks);
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
    orientToTopLeft(EPixelFormat format, std::vector<uint8_t>& data, nanogui::Vector2i size, EOrientation orientation, int priority) {
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

    std::vector<uint8_t> reorientedData(data.size());
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
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
                tlog::debug(
                ) << fmt::format("Image loader {} does not support loading {}: {} Trying next loader.", loadMethod, path, e.what());

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
        ++mLoadCounter;

        // null image pointers indicate failed loads. These shouldn't be pushed.
        if (!mPendingLoadedImages.top().images.empty()) {
            mLoadedImages.push(mPendingLoadedImages.top());
        }

        mPendingLoadedImages.pop();
        pushed = true;
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
