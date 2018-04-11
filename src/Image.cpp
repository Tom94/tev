// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <set>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

bool isExrFile(const string& filename) {
    ifstream f{filename, ios_base::binary};
    if (!f) {
        throw invalid_argument{tfm::format("File %s could not be opened.", filename)};
    }

    // Taken from http://www.openexr.com/ReadingAndWritingImageFiles.pdf
    char b[4];
    f.read(b, sizeof(b));
    return !!f && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;
}

atomic<int> Image::sId(0);

Image::Image(const string& filename, const string& channelSelector)
: mFilename{ filename }, mChannelSelector{ channelSelector }, mId{sId++} {
    if (!channelSelector.empty()) {
        mName = tfm::format("%s:%s", filename, channelSelector);
    } else {
        mName = filename;
    }

    if (isExrFile(filename)) {
        readExr();
    } else {
        readStbi();
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
                throw invalid_argument{tfm::format("Cannot obtain texture of %s:%s, because the channel does not exist.", filename(), channelName)};
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

string Image::toString() const {
    string result = tfm::format("Path: %s\n\nResolution: (%d, %d)\n\nChannels:\n", mName, size().x(), size().y());

    auto localLayers = mLayers;
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

void Image::readStbi() {
    // No exr image? Try our best using stbi
    cout << "Loading " << mFilename << " via STBI... " << flush;
    auto start = chrono::system_clock::now();

    ThreadPool threadPool;

    void* data;
    int numChannels;
    Vector2i size;
    bool isHdr = stbi_is_hdr(mFilename.c_str()) != 0;
    if (isHdr) {
        data = stbi_loadf(mFilename.c_str(), &size.x(), &size.y(), &numChannels, 0);
    } else {
        data = stbi_load(mFilename.c_str(), &size.x(), &size.y(), &numChannels, 0);
    }

    if (!data) {
        throw invalid_argument("Could not load texture data from file " + mFilename);
    }

    vector<Channel> channels;
    for (int c = 0; c < numChannels; ++c) {
        channels.emplace_back(c, size);
    }

    auto numPixels = (DenseIndex)size.x() * size.y();
    if (isHdr) {
        float* typedData = reinterpret_cast<float*>(data);
        threadPool.parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
            int baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                channels[c].at(i) = typedData[baseIdx + c];
            }
        });
    } else {
        unsigned char* typedData = reinterpret_cast<unsigned char*>(data);
        threadPool.parallelFor<DenseIndex>(0, numPixels, [&](DenseIndex i) {
            int baseIdx = i * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                channels[c].at(i) = toLinear((typedData[baseIdx + c]) / 255.0f);
            }
        });
    }

    stbi_image_free(data);

    for (auto& channel : channels) {
        string name = channel.name();
        if (matches(name, mChannelSelector, false)) {
            mChannels.emplace(move(name), move(channel));
        }
    }

    // STBI can not load layers, so all channels simply reside
    // within a topmost root layer.
    mLayers.emplace_back("");

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    cout << tfm::format("done after %.3f seconds.\n", elapsedSeconds.count());

    ensureValid();
}

void Image::readExr() {
    // OpenEXR for reading exr images
    cout << "Loading " << mFilename << " via OpenEXR... " << flush;
    auto start = chrono::system_clock::now();

    ThreadPool threadPool;

    Imf::MultiPartInputFile multiPartFile{mFilename.c_str()};
    int numParts = multiPartFile.parts();

    if (numParts <= 0) {
        throw invalid_argument{tfm::format("EXR image '%s' does not contain any parts.", mFilename)};
    }

    // Find the first part containing a channel that matches the given channelSubstr.
    int partIdx = 0;
    for (int i = 0; i < numParts; ++i) {
        Imf::InputPart part{multiPartFile, i};

        const Imf::ChannelList& imfChannels = part.header().channels();

        for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
            if (matches(c.name(), mChannelSelector, false)) {
                partIdx = i;
                goto l_foundPart;
            }
        }
    }
l_foundPart:

    Imf::InputPart file{multiPartFile, partIdx};
    Imath::Box2i dw = file.header().dataWindow();
    Vector2i size = {dw.max.x - dw.min.x + 1 , dw.max.y - dw.min.y + 1};

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
            // The code in this switch statement may seem overly complicated, but it helps
            // the compiler optimize. This code is time-critical for large images.
            switch (mImfChannel.type) {
                case Imf::HALF:
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&](DenseIndex i) {
                        channel.at(i) = static_cast<float>(*reinterpret_cast<const half*>(&mData[i * sizeof(half)]));
                    });
                    break;

                case Imf::FLOAT:
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&](DenseIndex i) {
                        channel.at(i) = *reinterpret_cast<const float*>(&mData[i * sizeof(float)]);
                    });
                    break;

                case Imf::UINT:
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&](DenseIndex i) {
                        channel.at(i) = static_cast<float>(*reinterpret_cast<const uint32_t*>(&mData[i * sizeof(uint32_t)]));
                    });
                    break;

                default:
                    throw runtime_error("Invalid pixel type encountered.");
            }
        }

        const string& name() const {
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
    set<string> layerNames;

    for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
        if (matches(c.name(), mChannelSelector, false)) {
            rawChannels.emplace_back(c.name(), c.channel().type);
            layerNames.insert(Channel::head(c.name()));
        }
    }

    if (rawChannels.empty()) {
        throw invalid_argument{tfm::format("No channels match '%s'.", mChannelSelector)};
    }

    for (const string& layer : layerNames) {
        mLayers.emplace_back(layer);
    }

    threadPool.parallelFor(0, (int)rawChannels.size(), [&](int i) {
        rawChannels[i].resize((DenseIndex)size.x() * size.y());
    });

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].registerWith(frameBuffer, dw);
    }

    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    for (const auto& rawChannel : rawChannels) {
        mChannels.emplace(rawChannel.name(), Channel{rawChannel.name(), size});
    }

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].copyTo(mChannels.at(rawChannels[i].name()), threadPool);
    }

    threadPool.waitUntilFinished();

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    cout << tfm::format("done after %.3f seconds.\n", elapsedSeconds.count());

    ensureValid();
}

void Image::ensureValid() {
    if (mLayers.empty()) {
        throw runtime_error{"Images must have at least one layer."};
    }

    if (mChannels.empty()) {
        throw runtime_error{"Images must have at least one channel."};
    }

    for (const auto& kv : mChannels) {
        if (kv.second.size() != size()) {
            throw runtime_error{tfm::format(
                "All channels must have the same size as their image. (%s:%dx%d != %dx%d)",
                kv.first, kv.second.size().x(), kv.second.size().y(), size().x(), size().y()
            )};
        }
    }
}

shared_ptr<Image> tryLoadImage(string filename, string channelSelector) {
    try {
        filename = absolutePath(filename);
    } catch (runtime_error e) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    try {
        return make_shared<Image>(filename, channelSelector);
    } catch (invalid_argument e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", filename, channelSelector, e.what());
    } catch (runtime_error e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", filename, channelSelector, e.what());
    } catch (Iex::BaseExc& e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", filename, channelSelector, e.what());
    }

    return nullptr;
}

TEV_NAMESPACE_END
