// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Image.h"

#include <ImfChannelList.h>
#include <ImfInputFile.h>

#include <stb_image.h>

#include <array>
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

void Image::readStbi(const std::string& filename) {
    // No exr image? Try our best using stbi
    cout << "Loading "s + filename + " via STBI." << endl;

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
        channels.emplace_back(name);
        channels.back().data().resize(numPixels);
    }

    for (size_t i = 0; i < numPixels; ++i) {
        size_t baseIdx = i * mNumChannels;
        for (size_t c = 0; c < mNumChannels; ++c) {
            channels[c].data()[i] = data[baseIdx + c];
        }
    }

    stbi_image_free(data);

    for (auto& channel : channels) {
        string name = channel.name();
        mChannels.emplace(move(name), move(channel));
    }

    // STBI can not load layers, so all channels simply reside
    // within a topmost root layer.
    mLayers.emplace_back("");
}

void Image::readExr(const std::string& filename) {
    // OpenEXR for reading exr images
    cout << "Loading "s + filename + " via OpenEXR." << endl;

    Imf::InputFile file(filename.c_str());
    Imath::Box2i dw = file.header().dataWindow();
    mSize.x() = dw.max.x - dw.min.x + 1;
    mSize.y() = dw.max.y - dw.min.y + 1;

    // Inline helper class for dealing with the raw channels loaded from an exr file.
    class RawChannel {
    public:
        RawChannel(string name, Imf::Channel imfChannel, size_t size)
        : mName(name), mImfChannel(imfChannel) {
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

        void copyTo(Channel& channel) const {
            int bpp = bytesPerPixel();
            auto& dstData = channel.data();
            dstData.resize(mData.size() / bpp);

            for (size_t i = 0; i < dstData.size(); ++i) {
                size_t rawIdx = i * bpp;
                switch (mImfChannel.type) {
                    case Imf::HALF:  dstData[i] = static_cast<float>(*reinterpret_cast<const half*>(&mData[rawIdx]));     break;
                    case Imf::FLOAT: dstData[i] = static_cast<float>(*reinterpret_cast<const float*>(&mData[rawIdx]));    break;
                    case Imf::UINT:  dstData[i] = static_cast<float>(*reinterpret_cast<const uint32_t*>(&mData[rawIdx])); break;
                    default:
                        throw runtime_error("Invalid pixel type encountered.");
                }
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
        rawChannels.emplace_back(i.name(), i.channel().type, mSize.prod());
        rawChannels.back().registerWith(frameBuffer, dw);
    }

    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    for (const auto& rawChannel : rawChannels) {
        mChannels.emplace(rawChannel.name(), Channel{rawChannel.name()});
        rawChannel.copyTo(mChannels.at(rawChannel.name()));
    }
}

TEV_NAMESPACE_END
