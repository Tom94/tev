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

#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/ImageLoader.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <fstream>
#include <istream>
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
    if (toRec709 == Matrix4f{1.0f}) {
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
                    auto rgb = toRec709 * Vector3f{r->at(i), g->at(i), b->at(i)};
                    r->at(i) = rgb.x();
                    g->at(i) = rgb.y();
                    b->at(i) = rgb.z();
                },
                priority
            )
        );
    }

    for (auto& task : tasks) {
        co_await task;
    }

    // Since the image data is now in Rec709 space, converting to Rec709 is the identity transform.
    toRec709 = Matrix4f{1.0f};
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
        shared_ptr<vector<float>> data;
        Vector2i size;

        struct Hash {
            size_t operator()(const DataDesc& interval) const { return hash<shared_ptr<vector<float>>>()(interval.data); }
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
        co_await tev::orientToTopLeft<float>(*c.data, c.size, orientation, priority);
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

    TEV_ASSERT(hasPremultipliedAlpha, "tev assumes an internal pre-multiplied-alpha representation.");
    TEV_ASSERT(toRec709 == Matrix4f{1.0f}, "tev assumes an images to be internally represented in sRGB/Rec709 space.");
}

atomic<int> Image::sId(0);

Image::dither_matrix_t Image::ditherMatrix() {
    // 8x8 Bayer dithering matrix scaled to [-0.5f, 0.5f] / 255
    dither_matrix_t thresholdMatrix = {
        {{0, 32, 8, 40, 2, 34, 10, 42},
         {48, 16, 56, 24, 50, 18, 58, 26},
         {12, 44, 4, 36, 14, 46, 6, 38},
         {60, 28, 52, 20, 62, 30, 54, 22},
         {3, 35, 11, 43, 1, 33, 9, 41},
         {51, 19, 59, 27, 49, 17, 57, 25},
         {15, 47, 7, 39, 13, 45, 5, 37},
         {63, 31, 55, 23, 61, 29, 53, 21}}
    };

    size_t nDisplayBits = 8;
    float scale = 1.0f / ((1 << nDisplayBits) - 1);
    for (size_t i = 0; i < DITHER_MATRIX_SIZE; ++i) {
        for (size_t j = 0; j < DITHER_MATRIX_SIZE; ++j) {
            thresholdMatrix[i][j] = (thresholdMatrix[i][j] / DITHER_MATRIX_SIZE / DITHER_MATRIX_SIZE - 0.5f) * scale;
        }
    }

    return thresholdMatrix;
}

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

bool Image::isInterleavedRgba(span<const string> channelNames) const {
    // It's fine if there are fewer than 4 channels -- they may still have been allocated as part of an interleaved RGBA buffer where some
    // of these 4 channels have default values. The following loop checks that the stride is 4 and that all present channels are adjacent.
    const float* interleavedData = nullptr;
    for (size_t i = 0; i < channelNames.size(); ++i) {
        const auto* chan = channel(channelNames[i]);
        if (!chan) {
            return false;
        }

        if (i == 0) {
            interleavedData = chan->data();
        }

        if (interleavedData != chan->data() - i || chan->stride() != 4) {
            return false;
        }
    }

    return interleavedData;
}

Texture* Image::texture(span<const string> channelNames, EInterpolationMode minFilter, EInterpolationMode magFilter) {
    if (size().x() > maxTextureSize() || size().y() > maxTextureSize()) {
        tlog::error() << fmt::format("{} is too large for Texturing. ({}x{})", mName, size().x(), size().y());
        return nullptr;
    }

    string lookup = fmt::format("{}-{}-{}", join(channelNames, ","), tev::toString(minFilter), tev::toString(magFilter));
    auto iter = mTextures.find(lookup);
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

    mTextures.emplace(
        lookup,
        ImageTexture{
            new Texture{
                        Texture::PixelFormat::RGBA,
                        Texture::ComponentFormat::Float32,
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

    bool directUpload = isInterleavedRgba(channelNames);

    tlog::debug() << fmt::format(
        "Uploading texture: direct={} filter={}-{} img={}:{}",
        directUpload,
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

        auto numPixels = this->numPixels();
        vector<float> data = vector<float>(numPixels * 4);

        vector<Task<void>> tasks;
        for (size_t i = 0; i < 4; ++i) {
            float defaultVal = i == 3 ? 1 : 0;
            if (i < channelNames.size()) {
                const auto* chan = channel(channelNames[i]);
                if (!chan) {
                    tasks.emplace_back(
                        ThreadPool::global().parallelForAsync<size_t>(
                            0, numPixels, [&data, defaultVal, i](size_t j) { data[j * 4 + i] = defaultVal; }, numeric_limits<int>::max()
                        )
                    );
                } else {
                    tasks.emplace_back(
                        ThreadPool::global().parallelForAsync<size_t>(
                            0, numPixels, [chan, &data, i](size_t j) { data[j * 4 + i] = chan->at(j); }, numeric_limits<int>::max()
                        )
                    );
                }
            } else {
                tasks.emplace_back(
                    ThreadPool::global().parallelForAsync<size_t>(
                        0, numPixels, [&data, defaultVal, i](size_t j) { data[j * 4 + i] = defaultVal; }, numeric_limits<int>::max()
                    )
                );
            }
        }

        waitAll<Task<void>>(tasks);
        texture->upload((uint8_t*)data.data());
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
        mChannelGroups.insert(
            mChannelGroups.begin() + (++groupPos),
            ChannelGroup{
                channel, {channel, channel, channel}
        }
        );
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
            if (groupChannels.size() == 1) {
                groupChannels.emplace_back(groupChannels.front());
                groupChannels.emplace_back(groupChannels.front());
            }

            if (hasAlpha) {
                groupChannels.emplace_back(alphaChannelName);
            }

            result.emplace_back(createChannelGroup(layerName, std::move(groupChannels)));
        }
    }

    for (const auto& name : allChannels) {
        if (hasAlpha) {
            result.emplace_back(createChannelGroup(layerName, vector<string>{name, name, name, alphaChannelName}));
        } else {
            result.emplace_back(createChannelGroup(layerName, vector<string>{name, name, name}));
        }
    }

    if (hasAlpha && result.empty()) {
        result.emplace_back(createChannelGroup(layerName, vector<string>{alphaChannelName, alphaChannelName, alphaChannelName}));
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
        vector<float> textureData(numPixels * 4);

        // Populate data for sub-region of the texture to be updated
        for (size_t i = 0; i < 4; ++i) {
            if (i < imageTexture.channels.size()) {
                const auto& localChannelName = imageTexture.channels[i];
                const auto* localChan = channel(localChannelName);
                TEV_ASSERT(localChan, "Channel to be updated must exist");

                for (int posY = 0; posY < height; ++posY) {
                    for (int posX = 0; posX < width; ++posX) {
                        size_t tileIdx = posX + posY * (size_t)width;
                        textureData[tileIdx * 4 + i] = localChan->at({x + posX, y + posY});
                    }
                }
            } else {
                float val = i == 3 ? 1 : 0;
                for (size_t j = 0; j < numPixels; ++j) {
                    textureData[j * 4 + i] = val;
                }
            }
        }

        imageTexture.nanoguiTexture->upload_sub_region((uint8_t*)textureData.data(), {x, y}, {width, height});
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

} // namespace tev
