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

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

vector<string> ImageData::channelsInLayer(string layerName) const {
    vector<string> result;

    if (layerName.empty()) {
        for (const auto& c : channels) {
            if (c.name().find(".") == string::npos) {
                result.emplace_back(c.name());
            }
        }
    } else {
        for (const auto& c : channels) {
            // If the layer name starts at the beginning, and
            // if no other dot is found after the end of the layer name,
            // then we have found a channel of this layer.
            if (c.name().starts_with(layerName) && c.name().length() > layerName.length()) {
                const auto& channelWithoutLayer = c.name().substr(layerName.length() + 1);
                if (channelWithoutLayer.find(".") == string::npos) {
                    result.emplace_back(c.name());
                }
            }
        }
    }

    return result;
}

void ImageData::alphaOperation(const function<void(Channel&, const Channel&)>& func) {
    for (const auto& layer : layers) {
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

Task<void> ImageData::multiplyAlpha(int priority) {
    vector<Task<void>> tasks;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        tasks.emplace_back(target.multiplyWithAsync(alpha, priority));
    });
    for (auto& task : tasks) {
        co_await task;
    }
}

Task<void> ImageData::unmultiplyAlpha(int priority) {
    vector<Task<void>> tasks;
    alphaOperation([&] (Channel& target, const Channel& alpha) {
        tasks.emplace_back(target.divideByAsync(alpha, priority));
    });
    for (auto& task : tasks) {
        co_await task;
    }
}

void ImageData::ensureValid() {
    if (layers.empty()) {
        throw runtime_error{"Images must have at least one layer."};
    }

    if (channels.empty()) {
        throw runtime_error{"Images must have at least one channel."};
    }

    for (const auto& c : channels) {
        if (c.size() != size()) {
            throw runtime_error{tfm::format(
                "All channels must have the same size as their image. (%s:%dx%d != %dx%d)",
                c.name(), c.size().x(), c.size().y(), size().x(), size().y()
            )};
        }
    }
}

atomic<int> Image::sId(0);

Image::Image(int id, const class path& path, ImageData&& data, const string& channelSelector)
: mPath{path}, mChannelSelector{channelSelector}, mData{std::move(data)}, mId{id} {
    mName = channelSelector.empty() ? path.str() : tfm::format("%s:%s", path, channelSelector);

    for (const auto& layer : mData.layers) {
        auto groups = getGroupedChannels(layer);
        mChannelGroups.insert(end(mChannelGroups), begin(groups), end(groups));
    }
    
    // Convert chromaticities to sRGB / Rec 709 if they aren't already.
    toRec709();
}

Image::~Image() {
    // Move the texture pointers to the main thread such that their reference count
    // hits zero there. This is required, because OpenGL calls must always happen
    // on the main thread.
    scheduleToMainThread([textures = std::move(mTextures)] {});
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

nanogui::Texture* Image::texture(const string& channelGroupName) {
    return texture(channelsInGroup(channelGroupName));
}

nanogui::Texture* Image::texture(const vector<string>& channelNames) {
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
        new nanogui::Texture{
            nanogui::Texture::PixelFormat::RGBA,
            nanogui::Texture::ComponentFormat::Float32,
            {size().x(), size().y()},
            nanogui::Texture::InterpolationMode::Trilinear,
            nanogui::Texture::InterpolationMode::Nearest,
            nanogui::Texture::WrapMode::ClampToEdge,
            1, nanogui::Texture::TextureFlags::ShaderRead,
            true,
        },
        channelNames,
        false,
    });
    auto& texture = mTextures.at(lookup).nanoguiTexture;

    auto numPixels = count();
    vector<float> data(numPixels * 4);

    vector<Task<void>> tasks;
    for (size_t i = 0; i < 4; ++i) {
        if (i < channelNames.size()) {
            const auto& channelName = channelNames[i];
            const auto* chan = channel(channelName);
            if (!chan) {
                throw invalid_argument{tfm::format("Cannot obtain texture of %s:%s, because the channel does not exist.", path(), channelName)};
            }

            const auto& channelData = chan->data();
            tasks.emplace_back(
                gThreadPool->parallelForAsync<DenseIndex>(0, numPixels, [&channelData, &data, i](DenseIndex j) {
                    data[j * 4 + i] = channelData(j);
                }, std::numeric_limits<int>::max())
            );
        } else {
            float val = i == 3 ? 1 : 0;
            tasks.emplace_back(
                gThreadPool->parallelForAsync<DenseIndex>(0, numPixels, [&data, val, i](DenseIndex j) {
                    data[j * 4 + i] = val;
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
            name = layer + "." + channelsString;
        } else {
            name = layer + ".(" + channelsString + ")";
        }

        return ChannelGroup{name, move(channels)};
    };

    string layerPrefix = layerName.empty() ? "" : (layerName + ".");
    string alphaChannelName = layerPrefix + "A";

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

        imageTexture.nanoguiTexture->upload_sub_region((uint8_t*)textureData.data(), {x, y}, {width, height});
        imageTexture.mipmapDirty = true;
    }
}

string Image::toString() const {
    string result = tfm::format("Path: %s\n\nResolution: (%d, %d)\n\nChannels:\n", mName, size().x(), size().y());

    auto localLayers = mData.layers;
    transform(begin(localLayers), end(localLayers), begin(localLayers), [this](string layer) {
        auto channels = mData.channelsInLayer(layer);
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

void Image::toRec709() {
    // No need to do anything for identity transforms
    if (mData.toRec709 == nanogui::Matrix4f{1.0f}) {
        return;
    }

    vector<future<void>> futures;

    for (const auto& layer : mData.layers) {
        string layerPrefix = layer.empty() ? "" : (layer + ".");

        Channel* r = nullptr;
        Channel* g = nullptr;
        Channel* b = nullptr;

        if (!(
            (r = mutableChannel(layerPrefix + "R")) && (g = mutableChannel(layerPrefix + "G")) && (b = mutableChannel(layerPrefix + "B")) ||
            (r = mutableChannel(layerPrefix + "r")) && (g = mutableChannel(layerPrefix + "g")) && (b = mutableChannel(layerPrefix + "b"))
        )) {
            // No RGB-triplet found
            continue;
        }

        TEV_ASSERT(r && g && b, "RGB triplet of channels must exist.");

        gThreadPool->parallelForAsync<DenseIndex>(0, r->count(), [r, g, b, this](DenseIndex i) {
            auto rgb = mData.toRec709 * nanogui::Vector3f{r->at(i), g->at(i), b->at(i)};
            r->at(i) = rgb.x();
            g->at(i) = rgb.y();
            b->at(i) = rgb.z();
        }, futures);
    }

    waitAll(futures);
}

Task<shared_ptr<Image>> tryLoadImage(int imageId, path path, istream& iStream, string channelSelector) {
    auto handleException = [&](const exception& e) {
        if (channelSelector.empty()) {
            tlog::error() << tfm::format("Could not load '%s'. %s", path, e.what());
        } else {
            tlog::error() << tfm::format("Could not load '%s:%s'. %s", path, channelSelector, e.what());
        }
    };

    try {
        auto start = chrono::system_clock::now();

        if (!iStream) {
            throw invalid_argument{tfm::format("Image %s could not be opened.", path)};
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
                int taskPriority = -imageId;

                loadMethod = imageLoader->name();
                auto [data, hasPremultipliedAlpha] = co_await imageLoader->load(iStream, path, channelSelector, taskPriority);
                data.ensureValid();

                // We assume an internal pre-multiplied-alpha representation
                if (!hasPremultipliedAlpha) {
                    co_await data.multiplyAlpha(taskPriority);
                }

                auto image = make_shared<Image>(imageId, path, std::move(data), channelSelector);

                auto end = chrono::system_clock::now();
                chrono::duration<double> elapsedSeconds = end - start;

                tlog::success() << tfm::format("Loaded '%s' via %s after %.3f seconds.", image->name(), loadMethod, elapsedSeconds.count());

                co_return image;
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

    co_return nullptr;
}

Task<shared_ptr<Image>> tryLoadImage(path path, istream& iStream, string channelSelector) {
    co_return co_await tryLoadImage(Image::drawId(), path, iStream, channelSelector);
}

Task<shared_ptr<Image>> tryLoadImage(int imageId, path path, string channelSelector) {
    try {
        path = path.make_absolute();
    } catch (const runtime_error&) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    ifstream fileStream{nativeString(path), ios_base::binary};
    co_return co_await tryLoadImage(imageId, path, fileStream, channelSelector);
}

Task<shared_ptr<Image>> tryLoadImage(path path, string channelSelector) {
    co_return co_await tryLoadImage(Image::drawId(), path, channelSelector);
}

void BackgroundImagesLoader::enqueue(const path& path, const string& channelSelector, bool shallSelect) {
    int imageId = Image::drawId();
    int loadId = mUnsortedLoadCounter++;

    gThreadPool->enqueueTask(coLambda([imageId, loadId, path, channelSelector, shallSelect, this]() -> Task<void> {
        auto image = co_await tryLoadImage(imageId, path, channelSelector);

        {
            std::lock_guard lock{mPendingLoadedImagesMutex};
            mPendingLoadedImages.push({ loadId, shallSelect, image });
        }

        if (publishSortedLoads()) {
            glfwPostEmptyEvent();
        }
    }), -imageId);
}

bool BackgroundImagesLoader::publishSortedLoads() {
    std::lock_guard lock{mPendingLoadedImagesMutex};
    bool pushed = false;
    while (!mPendingLoadedImages.empty() && mPendingLoadedImages.top().loadId == mLoadCounter) {
        ++mLoadCounter;

        // null image pointers indicate failed loads. These shouldn't be pushed.
        if (mPendingLoadedImages.top().image) {
            mLoadedImages.push(std::move(mPendingLoadedImages.top()));
        }

        mPendingLoadedImages.pop();
        pushed = true;
    }
    return pushed;
}

TEV_NAMESPACE_END
