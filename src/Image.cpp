// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/ThreadPool.h>

#include <Iex.h>

#include <chrono>
#include <iostream>

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

atomic<int> Image::sId(0);

Image::Image(const filesystem::path& path, const string& channelSelector)
: mPath{path}, mChannelSelector{channelSelector}, mId{sId++} {
    if (!channelSelector.empty()) {
        mName = tfm::format("%s:%s", path, channelSelector);
    } else {
        mName = path.str();
    }

    auto start = chrono::system_clock::now();

    ifstream f{nativeString(mPath), ios_base::binary};
    if (!f) {
        throw invalid_argument{tfm::format("File %s could not be opened.", mPath)};
    }

    std::string loadMethod;
    for (const auto& imageLoader : ImageLoader::getLoaders()) {
        // If we arrived at the last loader, then we want to at least try loading the image,
        // even if it is likely to fail.
        bool useLoader = imageLoader == ImageLoader::getLoaders().back() || imageLoader->canLoadFile(f);

        // Reset file cursor in case file load check changed it.
        f.seekg(0);

        if (useLoader) {
            loadMethod = imageLoader->name();
            mData = imageLoader->load(f, mPath, mChannelSelector);
            break;
        }
    }

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    tlog::success() << tfm::format("Loaded '%s' via %s after %.3f seconds.", mPath, loadMethod, elapsedSeconds.count());

    ensureValid();
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

const GlTexture* Image::texture(const vector<string>& channelNames) {
    string lookup = join(channelNames, ",");
    auto iter = mTextures.find(lookup);
    if (iter != end(mTextures)) {
        return &iter->second;
    }

    mTextures.emplace(lookup, GlTexture{});
    auto& texture = mTextures.at(lookup);

    auto numPixels = count();
    vector<float> data(numPixels * 4);

    ThreadPool pool;
    pool.parallelForNoWait(0, 4, [&](size_t i) {
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
    });
    pool.waitUntilFinished();

    texture.setData(data, size(), 4);
    return &texture;
}

vector<string> Image::channelsInLayer(string layerName) const {
    vector<string> result;

    if (layerName.empty()) {
        for (const auto& kv : mData.channels) {
            if (kv.first.find(".") == string::npos) {
                result.emplace_back(kv.first);
            }
        }
    } else {
        for (const auto& kv : mData.channels) {
            // If the layer name starts at the beginning, and
            // if no other dot is found after the end of the layer name,
            // then we have found a channel of this layer.
            if (kv.first.find(layerName) == 0 && kv.first.length() > layerName.length()) {
                const auto& channelWithoutLayer = kv.first.substr(layerName.length() + 1);
                if (channelWithoutLayer.find(".") == string::npos) {
                    result.emplace_back(kv.first);
                }
            }
        }
    }

    return result;
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

void Image::ensureValid() {
    if (mData.layers.empty()) {
        throw runtime_error{"Images must have at least one layer."};
    }

    if (mData.channels.empty()) {
        throw runtime_error{"Images must have at least one channel."};
    }

    for (const auto& kv : mData.channels) {
        if (kv.second.size() != size()) {
            throw runtime_error{tfm::format(
                "All channels must have the same size as their image. (%s:%dx%d != %dx%d)",
                kv.first, kv.second.size().x(), kv.second.size().y(), size().x(), size().y()
            )};
        }
    }
}

shared_ptr<Image> tryLoadImage(path path, string channelSelector) {
    try {
        path = path.make_absolute();
    } catch (runtime_error e) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    auto handleException = [&](const exception& e) {
        if (channelSelector.empty()) {
            tlog::error() << tfm::format("Could not load '%s'. %s", path, e.what());
        } else {
            tlog::error() << tfm::format("Could not load '%s:%s'. %s", path, channelSelector, e.what());
        }
    };

    try {
        return make_shared<Image>(path, channelSelector);
    } catch (const invalid_argument& e) {
        handleException(e);
    } catch (const runtime_error& e) {
        handleException(e);
    } catch (const Iex::BaseExc& e) {
        handleException(e);
    }

    return nullptr;
}

TEV_NAMESPACE_END
