// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#include "../include/Image.h"

#include <ImfChannelList.h>
#include <ImfInputFile.h>

#include <stb_image.h>

#include <array>
#include <iostream>

using namespace std;

namespace {
    bool endsWith(const string& a, const string& b) {
        return a.length() >= b.length() && a.compare(a.length() - b.length(), b.length(), b) == 0;
    }
}

Image::Image(const string& filename)
    : mName(filename) {
    if (endsWith(filename, ".exr")) {
        readExr(filename);
    } else {
        readStbi(filename);
    }
}

const GlTexture* Image::texture(const std::string& channelName) {
    auto iter = mTextures.find(channelName);
    if (iter != end(mTextures)) {
        return &iter->second;
    }

    const auto* chan = channel(channelName);
    if (!chan) {
        return nullptr;
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
        RawChannel(string name, Imf::PixelType type, size_t size)
            : mName(name), mType(type) {
            mData.resize(size * bytesPerPixel());
        }

        void registerWith(Imf::FrameBuffer& frameBuffer, const Imath::Box2i& dw) {
            int width = dw.max.x - dw.min.x + 1;
            frameBuffer.insert(mName.c_str(), Imf::Slice(
                mType,
                mData.data() - (dw.min.x - dw.min.y * width) * bytesPerPixel(),
                bytesPerPixel(), bytesPerPixel() * width, // stride in x and y
                1, 1, 0
            ));
        }

        void copyTo(Channel& channel) const {
            int bpp = bytesPerPixel();
            auto& dstData = channel.data();
            dstData.resize(mData.size() / bpp);

            for (size_t i = 0; i < dstData.size(); ++i) {
                size_t rawIdx = i * bpp;
                switch (mType) {
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
            return mType == Imf::HALF ? 2 : 4;
        }

        string mName;
        Imf::PixelType mType;
        vector<char> mData;
    };

    vector<RawChannel> rawChannels;
    Imf::FrameBuffer frameBuffer;

    const Imf::ChannelList& imfChannels = file.header().channels();
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
