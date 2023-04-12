// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/ThreadPool.h>

#include <Iex.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <fstream>
#include <istream>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

vector<string> ImageData::channelsInLayer(string layerName) const {
    vector<string> result;

    for (const auto& c : channels) {
        // If the layer name starts at the beginning, and
        // if no other dot is found after the end of the layer name,
        // then we have found a channel of this layer.
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

        if (!(
            ((r = mutableChannel(layer + "R")) && (g = mutableChannel(layer + "G")) && (b = mutableChannel(layer + "B"))) ||
            ((r = mutableChannel(layer + "r")) && (g = mutableChannel(layer + "g")) && (b = mutableChannel(layer + "b")))
        )) {
            // No RGB-triplet found
            continue;
        }

        TEV_ASSERT(r && g && b, "RGB triplet of channels must exist.");

        tasks.emplace_back(
            ThreadPool::global().parallelForAsync<size_t>(0, r->numPixels(), [r, g, b, this](size_t i) {
                auto rgb = toRec709 * Vector3f{r->at(i), g->at(i), b->at(i)};
                r->at(i) = rgb.x();
                g->at(i) = rgb.y();
                b->at(i) = rgb.z();
            }, priority)
        );
    }

    for (auto& task : tasks) {
        co_await task;
    }

    // Since the image data is now in Rec709 space,
    // converting to Rec709 is the identity transform.
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
        throw runtime_error{"Can't multiply with alpha twice."};
    }

    vector<Task<void>> tasks;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        tasks.emplace_back(target.multiplyWithAsync(alpha, priority));
    });
    for (auto& task : tasks) {
        co_await task;
    }

    hasPremultipliedAlpha = true;
}

Task<void> ImageData::unmultiplyAlpha(int priority) {
    if (!hasPremultipliedAlpha) {
        throw runtime_error{"Can't divide by alpha twice."};
    }

    vector<Task<void>> tasks;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        tasks.emplace_back(target.divideByAsync(alpha, priority));
    });
    for (auto& task : tasks) {
        co_await task;
    }

    hasPremultipliedAlpha = false;
}

Task<void> ImageData::ensureValid(const string& channelSelector, int taskPriority) {
    if (channels.empty()) {
        throw runtime_error{"Images must have at least one channel."};
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
            throw runtime_error{format(
                "All channels must have the same size as the data window. ({}:{}x{} != {}x{})",
                c.name(), c.size().x(), c.size().y(), size().x(), size().y()
            )};
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
        vector<Channel> tmp = move(channels);
        channels.clear();

        for (const auto& match : matches) {
            channels.emplace_back(move(tmp[match.second]));
        }
    }

    if (layers.empty()) {
        set<string> layerNames;
        for (auto& c : channels) {
            layerNames.insert(Channel::head(c.name()));
        }

        for (const string& l : layerNames) {
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

Image::Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, const string& channelSelector)
: mPath{path}, mFileLastModified{fileLastModified}, mChannelSelector{channelSelector}, mData{std::move(data)}, mId{Image::drawId()} {
    mName = channelSelector.empty() ? tev::toString(path) : format("{}:{}", tev::toString(path), channelSelector);

    for (const auto& l : mData.layers) {
        auto groups = getGroupedChannels(l);
        mChannelGroups.insert(end(mChannelGroups), begin(groups), end(groups));
    }
}

Image::~Image() {
    // Move the texture pointers to the main thread such that their reference count
    // hits zero there. This is required, because OpenGL calls must always happen
    // on the main thread.
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

Texture* Image::texture(const string& channelGroupName) {
    return texture(channelsInGroup(channelGroupName));
}

Texture* Image::texture(const vector<string>& channelNames) {
    string lookup = join(channelNames, ",");
    auto iter = mTextures.find(lookup);
    if (iter != end(mTextures)) {
        auto& texture = iter->second;
        if (texture.mipmapDirty) {
            texture.nanoguiTexture->generate_mipmap();
            texture.mipmapDirty = false;
        }
        return texture.nanoguiTexture.get();
    }

    mTextures.emplace(lookup, ImageTexture{
        new Texture{
            Texture::PixelFormat::RGBA,
            Texture::ComponentFormat::Float32,
            {size().x(), size().y()},
            Texture::InterpolationMode::Trilinear,
            Texture::InterpolationMode::Nearest,
            Texture::WrapMode::ClampToEdge,
            1, Texture::TextureFlags::ShaderRead,
            true,
        },
        channelNames,
        false,
    });
    auto& texture = mTextures.at(lookup).nanoguiTexture;

    auto numPixels = this->numPixels();
    vector<float> data(numPixels * 4);

    vector<Task<void>> tasks;
    for (size_t i = 0; i < 4; ++i) {
        float defaultVal = i == 3 ? 1 : 0;
        if (i < channelNames.size()) {
            const auto* chan = channel(channelNames[i]);
            if (!chan) {
                tasks.emplace_back(
                    ThreadPool::global().parallelForAsync<size_t>(0, numPixels, [&data, defaultVal, i](size_t j) {
                        data[j * 4 + i] = defaultVal;
                    }, std::numeric_limits<int>::max())
                );
            } else {
                const auto& channelData = chan->data();
                tasks.emplace_back(
                    ThreadPool::global().parallelForAsync<size_t>(0, numPixels, [&channelData, &data, i](size_t j) {
                        data[j * 4 + i] = channelData[j];
                    }, std::numeric_limits<int>::max())
                );
            }
        } else {
            tasks.emplace_back(
                ThreadPool::global().parallelForAsync<size_t>(0, numPixels, [&data, defaultVal, i](size_t j) {
                    data[j * 4 + i] = defaultVal;
                }, std::numeric_limits<int>::max())
            );
        }
    }
    waitAll(tasks);

    texture->upload((uint8_t*)data.data());
    texture->generate_mipmap();
    return texture.get();
}

vector<string> Image::channelsInGroup(const string& groupName) const {
    for (const auto& group : mChannelGroups) {
        if (group.name == groupName) {
            return group.channels;
        }
    }

    return {};
}

vector<ChannelGroup> Image::getGroupedChannels(const string& layerName) const {
    vector<vector<string>> groups = {
        { "R", "G", "B" },
        { "r", "g", "b" },
        { "X", "Y", "Z" },
        { "x", "y", "z" },
        { "U", "V" },
        { "u", "v" },
        { "Z" },
        { "z" },
    };

    auto createChannelGroup = [](string layer, vector<string> channels) {
        TEV_ASSERT(!channels.empty(), "Can't create a channel group without channels.");

        auto channelTails = channels;
        // Remove duplicates
        channelTails.erase(unique(begin(channelTails), end(channelTails)), end(channelTails));
        transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);
        string channelsString = join(channelTails, ",");

        string name;
        if (layer.empty()) {
            name = channelsString;
        } else if (channelTails.size() == 1) {
            name = layer + channelsString;
        } else {
            name = layer + "(" + channelsString + ")";
        }

        return ChannelGroup{name, move(channels)};
    };

    string alphaChannelName = layerName + "A";

    vector<string> allChannels = mData.channelsInLayer(layerName);

    auto alphaIt = find(begin(allChannels), end(allChannels), alphaChannelName);
    bool hasAlpha = alphaIt != end(allChannels);
    if (hasAlpha) {
        allChannels.erase(alphaIt);
    }

    vector<ChannelGroup> result;

    for (const auto& group : groups) {
        vector<string> groupChannels;
        for (const string& channel : group) {
            string name = layerName + channel;
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

            result.emplace_back(createChannelGroup(layerName, move(groupChannels)));
        }
    }

    for (const auto& name : allChannels) {
        if (hasAlpha) {
            result.emplace_back(
                createChannelGroup(layerName, vector<string>{name, name, name, alphaChannelName})
            );
        } else {
            result.emplace_back(
                createChannelGroup(layerName, vector<string>{name, name, name})
            );
        }
    }

    if (hasAlpha && result.empty()) {
        result.emplace_back(
            createChannelGroup(layerName, vector<string>{alphaChannelName, alphaChannelName, alphaChannelName})
        );
    }

    TEV_ASSERT(!result.empty(), "Images with no channels should never exist.");

    return result;
}

vector<string> Image::getSortedChannels(const string& layerName) const {
    string alphaChannelName = layerName + "A";

    bool includesAlphaChannel = false;

    vector<string> result;
    for (const auto& group : getGroupedChannels(layerName)) {
        for (auto name : group.channels) {
            if (name == alphaChannelName) {
                if (includesAlphaChannel) {
                    continue;
                }

                includesAlphaChannel = true;
            }
            result.emplace_back(name);
        }
    }

    return result;
}

vector<string> Image::getExistingChannels(const vector<string>& requestedChannels) const {
    vector<string> result;
    std::copy_if(std::begin(requestedChannels), std::end(requestedChannels), std::back_inserter(result),
        [&](const string& c) {
            return hasChannel(c);
        }
    );
    return result;
}

void Image::updateChannel(const string& channelName, int x, int y, int width, int height, const vector<float>& data) {
    Channel* chan = mutableChannel(channelName);
    if (!chan) {
        tlog::warning() << "Channel " << channelName << " could not be updated, because it does not exist.";
        return;
    }

    chan->updateTile(x, y, width, height, data);

    // Update textures that are cached for this channel
    for (auto& kv : mTextures) {
        auto& imageTexture = kv.second;
        if (find(begin(imageTexture.channels), end(imageTexture.channels), channelName) == end(imageTexture.channels)) {
            continue;
        }

        auto numPixels = (size_t)width * height;
        vector<float> textureData(numPixels * 4);

        // Populate data for sub-region of the texture to be updated
        for (size_t i = 0; i < 4; ++i) {
            if (i < imageTexture.channels.size()) {
                const auto& localChannelName = imageTexture.channels[i];
                const auto* localChan = channel(localChannelName);
                TEV_ASSERT(localChan, "Channel to be updated must exist");

                for (int posY = 0; posY < height; ++posY) {
                    for (int posX = 0; posX < width; ++posX) {
                        int tileIdx = posX + posY * width;
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

void Image::updateVectorGraphics(bool append, const vector<VgCommand>& commands) {
    if (!append) {
        mVgCommands.clear();
    }

    std::copy(std::begin(commands), std::end(commands), std::back_inserter(mVgCommands));
}

template <typename T>
time_t to_time_t(T timePoint) {
    // `clock_cast` appears to throw errors on some systems, so we're using this slightly hacky
    // inaccurate/random time conversion (now() is not called simultaneously for both clocks)
    // in order to convert to system time.
    using namespace chrono;
    return system_clock::to_time_t(time_point_cast<system_clock::duration>(timePoint - T::clock::now() + system_clock::now()));
}

string Image::toString() const {
    stringstream sstream;
    sstream << mName << "\n\n";

    {
        time_t cftime = to_time_t(mFileLastModified);
        sstream << "Last modified:\n" << asctime(localtime(&cftime)) << "\n";
    }

    sstream << "Resolution: (" << size().x() << ", " << size().y() << ")\n";
    if (displayWindow() != dataWindow() || displayWindow().min != Vector2i{0}) {
        sstream << "Display window: (" << displayWindow().min.x() << ", " << displayWindow().min.y() << ")(" << displayWindow().max.x() << ", " << displayWindow().max.y() << ")\n";
        sstream << "Data window: (" << dataWindow().min.x() << ", " << dataWindow().min.y() << ")(" << dataWindow().max.x() << ", " << dataWindow().max.y() << ")\n";
    }

    sstream << "\nChannels:\n";

    auto localLayers = mData.layers;
    transform(begin(localLayers), end(localLayers), begin(localLayers), [this](string layer) {
        auto channels = mData.channelsInLayer(layer);
        transform(begin(channels), end(channels), begin(channels), [](string channel) {
            return Channel::tail(channel);
        });
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

Task<vector<shared_ptr<Image>>> tryLoadImage(int taskPriority, fs::path path, istream& iStream, string channelSelector) {
    auto handleException = [&](const exception& e) {
        if (channelSelector.empty()) {
            tlog::error() << format("Could not load {}. {}", toString(path), e.what());
        } else {
            tlog::error() << format("Could not load {}:{}. {}", toString(path), channelSelector, e.what());
        }
    };

    // No need to keep loading images if tev is already shutting down again.
    if (shuttingDown()) {
        co_return {};
    }

    try {
        auto start = chrono::system_clock::now();

        if (!iStream) {
            throw invalid_argument{format("Image {} could not be opened.", path)};
        }

        fs::file_time_type fileLastModified = fs::file_time_type::clock::now();
        if (fs::exists(path)) {
            // Unlikely, but the file could have been deleted, moved, or something
            // else might have happened to it that makes obtaining its last modified
            // time impossible. Ignore such errors.
            try {
                fileLastModified = fs::last_write_time(path);
            } catch (...) {}
        }

        std::string loadMethod;
        for (const auto& imageLoader : ImageLoader::getLoaders()) {
            // If we arrived at the last loader, then we want to at least try loading the image,
            // even if it is likely to fail.
            bool useLoader = imageLoader == ImageLoader::getLoaders().back() || imageLoader->canLoadFile(iStream);

            // Reset file cursor in case file load check changed it.
            iStream.clear();
            iStream.seekg(0);

            if (useLoader) {
                // Earlier images should be prioritized when loading.
                loadMethod = imageLoader->name();
                auto imageData = co_await imageLoader->load(iStream, path, channelSelector, taskPriority);

                vector<shared_ptr<Image>> images;
                for (auto& i : imageData) {
                    co_await i.ensureValid(channelSelector, taskPriority);

                    // If multiple image "parts" were loaded and they have names,
                    // ensure that these names are present in the channel selector.
                    string localChannelSelector = channelSelector;
                    if (!i.partName.empty()) {
                        auto selectorParts = split(channelSelector, ",");
                        if (channelSelector.empty()) {
                            localChannelSelector = i.partName;
                        } else if (find(begin(selectorParts), end(selectorParts), i.partName) == end(selectorParts)) {
                            localChannelSelector = join(vector<string>{i.partName, channelSelector}, ",");
                        }
                    }

                    images.emplace_back(make_shared<Image>(path, fileLastModified, std::move(i), localChannelSelector));
                }

                auto end = chrono::system_clock::now();
                chrono::duration<double> elapsedSeconds = end - start;

                tlog::success() << format("Loaded {} via {} after {:.3f} seconds.", toString(path), loadMethod, elapsedSeconds.count());

                co_return images;
            }
        }

        throw runtime_error{"No suitable image loader found."};
    } catch (const invalid_argument& e) {
        handleException(e);
    } catch (const runtime_error& e) {
        handleException(e);
    } catch (const Iex::BaseExc& e) {
        handleException(e);
    } catch (const future_error& e) {
        handleException(e);
    }

    co_return {};
}

Task<vector<shared_ptr<Image>>> tryLoadImage(fs::path path, istream& iStream, string channelSelector) {
    co_return co_await tryLoadImage(-Image::drawId(), path, iStream, channelSelector);
}

Task<vector<shared_ptr<Image>>> tryLoadImage(int taskPriority, fs::path path, string channelSelector) {
    try {
        path = fs::absolute(path);
    } catch (const runtime_error&) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    ifstream fileStream{path, ios_base::binary};
    co_return co_await tryLoadImage(taskPriority, path, fileStream, channelSelector);
}

Task<vector<shared_ptr<Image>>> tryLoadImage(fs::path path, string channelSelector) {
    co_return co_await tryLoadImage(-Image::drawId(), path, channelSelector);
}

void BackgroundImagesLoader::enqueue(const fs::path& path, const string& channelSelector, bool shallSelect, const shared_ptr<Image>& toReplace) {
    // If we're trying to open a directory, try loading all the images inside of that directory
    if (fs::exists(path) && fs::is_directory(path)) {
        tlog::info() << "Loading images " << (mRecursiveDirectories ? "recursively " : "") << "from directory " << toString(path);

        fs::path canonicalPath = fs::canonical(path);
        mDirectories[canonicalPath].emplace(channelSelector);

        bool first = true;
        forEachFileInDir(mRecursiveDirectories, canonicalPath, [&](auto const& entry) {
            if (!entry.is_directory()) {
                mFilesFoundInDirectories.emplace(PathAndChannelSelector{entry, channelSelector});
                enqueue(entry, channelSelector, first ? shallSelect : false);
                first = false;
            }
        });
        return;
    }

    int loadId = mUnsortedLoadCounter++;
    invokeTaskDetached([loadId, path, channelSelector, shallSelect, toReplace, this]() -> Task<void> {
        int taskPriority = -Image::drawId();

        co_await ThreadPool::global().enqueueCoroutine(taskPriority);
        auto images = co_await tryLoadImage(taskPriority, path, channelSelector);

        {
            std::lock_guard lock{mPendingLoadedImagesMutex};
            mPendingLoadedImages.push({ loadId, shallSelect, images, toReplace });
        }

        if (publishSortedLoads()) {
            redrawWindow();
        }
    });
}

void BackgroundImagesLoader::checkDirectoriesForNewFilesAndLoadThose() {
    for (const auto& dir : mDirectories) {
        forEachFileInDir(mRecursiveDirectories, dir.first, [&](auto const& entry) {
            if (!entry.is_directory()) {
                for (const auto& channelSelector : dir.second) {
                    PathAndChannelSelector p = {entry, channelSelector};
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
    std::lock_guard lock{mPendingLoadedImagesMutex};
    bool pushed = false;
    while (!mPendingLoadedImages.empty() && mPendingLoadedImages.top().loadId == mLoadCounter) {
        ++mLoadCounter;

        // null image pointers indicate failed loads. These shouldn't be pushed.
        if (!mPendingLoadedImages.top().images.empty()) {
            mLoadedImages.push(std::move(mPendingLoadedImages.top()));
        }

        mPendingLoadedImages.pop();
        pushed = true;
    }
    return pushed;
}

TEV_NAMESPACE_END
