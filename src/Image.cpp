// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/ThreadPool.h>

#include <Iex.h>

#include <chrono>
#include <fstream>
#include <istream>

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

atomic<int> Image::sId(0);

Image::Image(const class path& path, istream& iStream, const string& channelSelector)
: mPath{path}, mChannelSelector{channelSelector}, mId{sId++} {
    mName = channelSelector.empty() ? path.str() : tfm::format("%s:%s", path, channelSelector);

    auto start = chrono::system_clock::now();

    if (!iStream) {
        throw invalid_argument{tfm::format("Image %s could not be opened.", mName)};
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
            loadMethod = imageLoader->name();
            bool hasPremultipliedAlpha = false;
            mData = imageLoader->load(iStream, mPath, mChannelSelector, hasPremultipliedAlpha);
            ensureValid();

            // We assume an internal pre-multiplied-alpha representation
            if (!hasPremultipliedAlpha) {
                multiplyAlpha();
            }
            break;
        }
    }

    for (const auto& layer : mData.layers) {
        auto groups = getGroupedChannels(layer);
        mChannelGroups.insert(end(mChannelGroups), begin(groups), end(groups));
    }

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    ensureValid();

    tlog::success() << tfm::format("Loaded '%s' via %s after %.3f seconds.", mName, loadMethod, elapsedSeconds.count());
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

GlTexture* Image::texture(const string& channelGroupName) {
    return texture(channelsInGroup(channelGroupName));
}

GlTexture* Image::texture(const vector<string>& channelNames) {
    string lookup = join(channelNames, ",");
    auto iter = mTextures.find(lookup);
    if (iter != end(mTextures)) {
        return &iter->second.glTexture;
    }

    mTextures.emplace(lookup, ImageTexture{GlTexture{}, channelNames});
    auto& texture = mTextures.at(lookup).glTexture;

    auto numPixels = count();
    vector<float> data(numPixels * 4);

    ThreadPool pool;
    for (size_t i = 0; i < 4; ++i) {
        if (i < channelNames.size()) {
            const auto& channelName = channelNames[i];
            const auto* chan = channel(channelName);
            if (!chan) {
                throw invalid_argument{tfm::format("Cannot obtain texture of %s:%s, because the channel does not exist.", path(), channelName)};
            }

            const auto& channelData = chan->data();
            pool.parallelForNoWait<DenseIndex>(0, numPixels, [&channelData, &data, i](DenseIndex j) {
                data[j * 4 + i] = channelData(j);
            });
        } else {
            float val = i == 3 ? 1 : 0;
            pool.parallelForNoWait<DenseIndex>(0, numPixels, [&data, val, i](DenseIndex j) {
                data[j * 4 + i] = val;
            });
        }
    }
    pool.waitUntilFinished();

    texture.setData(data, size(), 4);
    return &texture;
}

vector<string> Image::channelsInLayer(string layerName) const {
    vector<string> result;

    if (layerName.empty()) {
        for (const auto& c : mData.channels) {
            if (c.name().find(".") == string::npos) {
                result.emplace_back(c.name());
            }
        }
    } else {
        for (const auto& c : mData.channels) {
            // If the layer name starts at the beginning, and
            // if no other dot is found after the end of the layer name,
            // then we have found a channel of this layer.
            if (c.name().find(layerName) == 0 && c.name().length() > layerName.length()) {
                const auto& channelWithoutLayer = c.name().substr(layerName.length() + 1);
                if (channelWithoutLayer.find(".") == string::npos) {
                    result.emplace_back(c.name());
                }
            }
        }
    }

    return result;
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
            name = layer + "." + channelsString;
        } else {
            name = layer + ".(" + channelsString + ")";
        }

        return ChannelGroup{name, move(channels)};
    };

    string layerPrefix = layerName.empty() ? "" : (layerName + ".");
    string alphaChannelName = layerPrefix + "A";

    vector<string> allChannels = channelsInLayer(layerName);

    auto alphaIt = find(begin(allChannels), end(allChannels), alphaChannelName);
    bool hasAlpha = alphaIt != end(allChannels);
    if (hasAlpha) {
        allChannels.erase(alphaIt);
    }

    vector<ChannelGroup> result;

    for (const auto& group : groups) {
        vector<string> groupChannels;
        for (const string& channel : group) {
            string name = layerPrefix + channel;
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
    string layerPrefix = layerName.empty() ? "" : (layerName + ".");
    string alphaChannelName = layerPrefix + "A";
    
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

        auto numPixels = width * height;
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
                for (DenseIndex j = 0; j < numPixels; ++j) {
                    textureData[j * 4 + i] = val;
                }
            }
        }

        imageTexture.glTexture.setDataSub(textureData, {x, y}, {width, height}, 4);
    }
}

string Image::toString() const {
    string result = tfm::format("Path: %s\n\nResolution: (%d, %d)\n\nChannels:\n", mName, size().x(), size().y());

    auto localLayers = mData.layers;
    transform(begin(localLayers), end(localLayers), begin(localLayers), [this](string layer) {
        auto channels = channelsInLayer(layer);
        transform(begin(channels), end(channels), begin(channels), [](string channel) {
            return Channel::tail(channel);
        });
        if (layer.empty()) {
            layer = "<root>";
        }
        return layer + ": " + join(channels, ",");
    });

    return result + join(localLayers, "\n");
}

void Image::alphaOperation(const function<void(Channel&, const Channel&)>& func) {
    for (const auto& layer : mData.layers) {
        string layerPrefix = layer.empty() ? "" : (layer + ".");
        string alphaChannelName = layerPrefix + "A";

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

void Image::multiplyAlpha() {
    ThreadPool threadPool;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        target.multiplyWithAsync(alpha, threadPool);
    });
    threadPool.waitUntilFinished();
}

void Image::unmultiplyAlpha() {
    ThreadPool threadPool;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        target.divideByAsync(alpha, threadPool);
    });
    threadPool.waitUntilFinished();
}

void Image::ensureValid() {
    if (mData.layers.empty()) {
        throw runtime_error{"Images must have at least one layer."};
    }

    if (mData.channels.empty()) {
        throw runtime_error{"Images must have at least one channel."};
    }

    for (const auto& c : mData.channels) {
        if (c.size() != size()) {
            throw runtime_error{tfm::format(
                "All channels must have the same size as their image. (%s:%dx%d != %dx%d)",
                c.name(), c.size().x(), c.size().y(), size().x(), size().y()
            )};
        }
    }
}

shared_ptr<Image> tryLoadImage(path path, istream& iStream, string channelSelector) {
    auto handleException = [&](const exception& e) {
        if (channelSelector.empty()) {
            tlog::error() << tfm::format("Could not load '%s'. %s", path, e.what());
        } else {
            tlog::error() << tfm::format("Could not load '%s:%s'. %s", path, channelSelector, e.what());
        }
    };

    try {
        return make_shared<Image>(path, iStream, channelSelector);
    } catch (const invalid_argument& e) {
        handleException(e);
    } catch (const runtime_error& e) {
        handleException(e);
    } catch (const Iex::BaseExc& e) {
        handleException(e);
    }

    return nullptr;
}

shared_ptr<Image> tryLoadImage(path path, string channelSelector) {
    try {
        path = path.make_absolute();
    } catch (runtime_error e) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    ifstream fileStream{nativeString(path), ios_base::binary};
    return tryLoadImage(path, fileStream, channelSelector);
}

void BackgroundImagesLoader::enqueue(const path& path, const string& channelSelector, bool shallSelect) {
    mWorkers.enqueueTask([path, channelSelector, shallSelect, this] {
        auto image = tryLoadImage(path, channelSelector);
        if (image) {
            mLoadedImages.push({ shallSelect, image });
        }

        glfwPostEmptyEvent();
    });
}

TEV_NAMESPACE_END
