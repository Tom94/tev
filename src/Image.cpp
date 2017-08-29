// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Image.h"
#include "../include/ThreadPool.h"

#include <ImfChannelList.h>
#include <ImfInputFile.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <set>

using namespace std;

TEV_NAMESPACE_BEGIN

bool isExrFile(const string& filename) {
    std::ifstream f{filename, std::ios_base::binary};
    if (!f) {
        throw invalid_argument{tfm::format("File %s could not be opened.", filename)};
    }

    // Taken from http://www.openexr.com/ReadingAndWritingImageFiles.pdf
    char b[4];
    f.read(b, sizeof(b));
    return !!f && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;
}

Image::Image(const string& filename)
: mName(filename) {
    if (isExrFile(filename)) {
        readExr(filename);
    } else {
        readStbi(filename);
    }
}

string Image::shortName() {
    size_t slashPosition = mName.find_last_of("/\\");
    if (slashPosition != string::npos) {
        return mName.substr(slashPosition + 1);
    }

    return mName;
}

vector<string> Image::channelsInLayer(string layerName) const {
    vector<string> result;

    if (layerName.empty()) {
        for (const auto& kv : mChannels) {
            if (kv.first.find(".") == string::npos) {
                result.emplace_back(kv.first);
            }
        }
    } else {
        for (const auto& kv : mChannels) {
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

const GlTexture* Image::texture(const std::string& channelName) {
    auto iter = mTextures.find(channelName);
    if (iter != end(mTextures)) {
        return &iter->second;
    }

    const auto* chan = channel(channelName);
    if (!chan) {
        throw invalid_argument{tfm::format("Cannot obtain texture of channel %s, because the channel does not exist.", channelName)};
    }

    mTextures.emplace(channelName, GlTexture{});
    auto& texture = mTextures.at(channelName);
    texture.setData(chan->data(), mSize, 1);

    return &texture;
}

string Image::toString() const {
    string result = tfm::format("Path: %s\n\nResolution: (%d, %d)\n\nChannels:\n", mName, mSize.x(), mSize.y());

    auto localLayers = mLayers;
    transform(begin(localLayers), end(localLayers), begin(localLayers), [this](string layer) {
        auto channels = channelsInLayer(layer);
        transform(begin(channels), end(channels), begin(channels), [this](string channel) {
            return Channel::tail(channel);
        });
        if (layer.empty()) {
            layer = "<root>";
        }
        return layer + ": " + join(channels, ",");
    });

    return result + join(localLayers, "\n");
}

void Image::readStbi(const std::string& filename) {
    // No exr image? Try our best using stbi
    cout << "Loading "s + filename + " via STBI... ";
    auto start = chrono::system_clock::now();

    ThreadPool threadPool;

    int numChannels;
    auto data = stbi_loadf(filename.c_str(), &mSize.x(), &mSize.y(), &numChannels, 0);
    if (!data) {
        throw invalid_argument("Could not load texture data from file " + filename);
    }

    mNumChannels = static_cast<size_t>(numChannels);
    size_t numPixels = mSize.prod();

    vector<string> channelNames = {"R", "G", "B", "A"};

    vector<Channel> channels;
    for (size_t c = 0; c < mNumChannels; ++c) {
        string name = c < channelNames.size() ? channelNames[c] : to_string(c - channelNames.size());
        channels.emplace_back(name, mSize);
        channels.back().data().resize(numPixels);
    }

    threadPool.parallelFor(0, numPixels, [&](size_t i) {
        size_t baseIdx = i * mNumChannels;
        for (size_t c = 0; c < mNumChannels; ++c) {
            channels[c].data()[i] = data[baseIdx + c];
        }
    });

    stbi_image_free(data);

    for (auto& channel : channels) {
        string name = channel.name();
        mChannels.emplace(move(name), move(channel));
    }

    // STBI can not load layers, so all channels simply reside
    // within a topmost root layer.
    mLayers.emplace_back("");

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    cout << tfm::format("done after %.3f seconds.\n", elapsedSeconds.count());
}

void Image::readExr(const std::string& filename) {
    // OpenEXR for reading exr images
    cout << "Loading "s + filename + " via OpenEXR... ";
    auto start = chrono::system_clock::now();

    ThreadPool threadPool;

    Imf::InputFile file(filename.c_str());
    Imath::Box2i dw = file.header().dataWindow();
    mSize.x() = dw.max.x - dw.min.x + 1;
    mSize.y() = dw.max.y - dw.min.y + 1;

    // Inline helper class for dealing with the raw channels loaded from an exr file.
    class RawChannel {
    public:
        RawChannel(string name, Imf::Channel imfChannel)
        : mName(name), mImfChannel(imfChannel) {
        }

        void resize(size_t size) {
            mData.resize(size * bytesPerPixel());
        }

        void registerWith(Imf::FrameBuffer& frameBuffer, const Imath::Box2i& dw) {
            int width = dw.max.x - dw.min.x + 1;

            frameBuffer.insert(mName.c_str(), Imf::Slice(
                mImfChannel.type,
                mData.data() - (dw.min.x + dw.min.y * width) * bytesPerPixel(),
                bytesPerPixel(), bytesPerPixel() * width,
                mImfChannel.xSampling, mImfChannel.ySampling, 0
            ));
        }

        void copyTo(Channel& channel, ThreadPool& threadPool) const {
            auto& dstData = channel.data();
            dstData.resize(mData.size() / bytesPerPixel());

            // The code in this switch statement may seem overly complicated, but it helps
            // the compiler optimize. This code is time-critical for large images.
            switch (mImfChannel.type) {
                case Imf::HALF:
                    threadPool.parallelForNoWait(0, dstData.size(), [&](size_t i) {
                        dstData[i] = static_cast<float>(*reinterpret_cast<const half*>(&mData[i * sizeof(half)]));
                    });
                    break;

                case Imf::FLOAT:
                    threadPool.parallelForNoWait(0, dstData.size(), [&](size_t i) {
                        dstData[i] = *reinterpret_cast<const float*>(&mData[i * sizeof(float)]);
                    });
                    break;

                case Imf::UINT:
                    threadPool.parallelForNoWait(0, dstData.size(), [&](size_t i) {
                        dstData[i] = static_cast<float>(*reinterpret_cast<const uint32_t*>(&mData[i * sizeof(uint32_t)]));
                    });
                    break;

                default:
                    throw runtime_error("Invalid pixel type encountered.");
            }
        }

        const auto& name() const {
            return mName;
        }

    private:
        int bytesPerPixel() const {
            switch (mImfChannel.type) {
                case Imf::HALF:  return sizeof(half);
                case Imf::FLOAT: return sizeof(float);
                case Imf::UINT:  return sizeof(uint32_t);
                default:
                    throw runtime_error("Invalid pixel type encountered.");
            }
        }

        string mName;
        Imf::Channel mImfChannel;
        vector<char> mData;
    };

    vector<RawChannel> rawChannels;
    Imf::FrameBuffer frameBuffer;

    const Imf::ChannelList& imfChannels = file.header().channels();

    // The topmost root layer isn't included in OpenEXRs layer list.
    mLayers.emplace_back("");
    set<string> layerNames;
    imfChannels.layers(layerNames);
    for (const string& layer : layerNames) {
        mLayers.emplace_back(layer);
    }

    for (Imf::ChannelList::ConstIterator i = imfChannels.begin(); i != imfChannels.end(); ++i) {
        rawChannels.emplace_back(i.name(), i.channel().type);
    }

    threadPool.parallelFor(0, rawChannels.size(), [&](size_t i) {
        rawChannels[i].resize(mSize.prod());
    });

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].registerWith(frameBuffer, dw);
    }

    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    for (const auto& rawChannel : rawChannels) {
        mChannels.emplace(rawChannel.name(), Channel{rawChannel.name(), mSize});
    }

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].copyTo(mChannels.at(rawChannels[i].name()), threadPool);
    }

    threadPool.waitUntilFinished();

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    cout << tfm::format("done after %.3f seconds.\n", elapsedSeconds.count());
}

shared_ptr<Image> tryLoadImage(string filename) {
    try {
        return make_shared<Image>(filename);
    } catch (invalid_argument e) {
        tfm::format(cerr, "Could not load image from %s: %s\n", filename, e.what());
    } catch (Iex::BaseExc& e) {
        tfm::format(cerr, "Could not load image from %s: %s\n", filename, e.what());
    }

    return nullptr;
}

TEV_NAMESPACE_END
