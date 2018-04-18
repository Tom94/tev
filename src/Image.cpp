// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Image.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStdIO.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <set>

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

bool isExrFile(ifstream& f) {
    // Taken from http://www.openexr.com/ReadingAndWritingImageFiles.pdf
    char b[4];
    f.read(b, sizeof(b));
    if (f.gcount() < 4) {
        return false;
    }

    bool result = !!f && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;
    f.seekg(0);
    return result;
}

bool isPfmFile(ifstream& f) {
    char b[2];
    f.read(b, sizeof(b));
    if (f.gcount() < 2) {
        return false;
    }

    bool result = !!f && b[0] == 'P' && (b[1] == 'F' || b[1] == 'f');
    f.seekg(0);
    return result;
}

atomic<int> Image::sId(0);

Image::Image(const filesystem::path& path, const string& channelSelector)
: mPath{path}, mChannelSelector{channelSelector}, mId{sId++} {
    if (!channelSelector.empty()) {
        mName = tfm::format("%s:%s", path, channelSelector);
    } else {
        mName = path.str();
    }

    cout << "Loading " << mPath;
    auto start = chrono::system_clock::now();

    ifstream f{nativeString(mPath), ios_base::binary};
    if (!f) {
        throw invalid_argument{tfm::format("File %s could not be opened.", mPath)};
    }

    if (isExrFile(f)) {
        cout << " via OpenEXR... " << flush;
        readExr(f);
    } else if (isPfmFile(f)) {
        cout << " via PFM... " << flush;
        readPfm(f);
    } else {
        cout << " via STBI... " << flush;
        readStbi(f);
    }

    auto end = chrono::system_clock::now();
    chrono::duration<double> elapsedSeconds = end - start;

    cout << tfm::format("done after %.3f seconds.\n", elapsedSeconds.count());

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

void Image::readPfm(ifstream& f) {
    ThreadPool threadPool;

    string magic;
    Vector2i size;
    float scale;

    f >> magic >> size.x() >> size.y() >> scale;

    if (magic != "PF" && magic != "Pf") {
        throw invalid_argument{tfm::format("Invalid magic PFM string %s in file %s", magic, mPath)};
    }

    if (!isfinite(scale) || scale == 0) {
        throw invalid_argument{tfm::format("Invalid PFM scale %f in file %s", scale, mPath)};
    }

    int numChannels = magic[1] == 'F' ? 3 : 1;
    bool isPfmLittleEndian = scale < 0;
    scale = abs(scale);

    vector<Channel> channels;
    for (int c = 0; c < numChannels; ++c) {
        channels.emplace_back(c, size);
    }

    auto numPixels = (DenseIndex)size.x() * size.y();
    auto numFloats = numPixels * numChannels;
    auto numBytes = numFloats * sizeof(float);

    // Stop eating new lines in binary mode.
    f.unsetf(std::ios::skipws);
    // Skip last newline at the end of the header.
    f.seekg(1, ios_base::cur);

    // Read entire file in binary mode.
    vector<float> data(numFloats);
    f.read(reinterpret_cast<char*>(data.data()), numBytes);
    if (f.gcount() < (streamsize)numBytes) {
        throw invalid_argument{tfm::format("Not sufficient bytes to read (%d vs %d) in file %s", f.gcount(), numBytes, mPath)};
    }

    // Reverse bytes of every float if endianness does not match up with system
    const bool shallSwapBytes = isSystemLittleEndian() != isPfmLittleEndian;

    auto typedData = reinterpret_cast<const float*>(data.data());
    threadPool.parallelFor<DenseIndex>(0, size.y(), [&](DenseIndex y) {
        for (int x = 0; x < size.x(); ++x) {
            int baseIdx = (y * size.x() + x) * numChannels;
            for (int c = 0; c < numChannels; ++c) {
                float val = typedData[baseIdx + c];

                // Thankfully, due to branch prediction, the "if" in the
                // inner loop is no significant overhead.
                if (shallSwapBytes) {
                    val = swapBytes(val);
                }

                // Flip image vertically due to PFM format
                channels[c].at({x, size.y() - y - 1}) = scale * val;
            }
        }
    });

    for (auto& channel : channels) {
        string name = channel.name();
        if (matches(name, mChannelSelector, false)) {
            mChannels.emplace(move(name), move(channel));
        }
    }

    // PFM can not contain layers, so all channels simply reside
    // within a topmost root layer.
    mLayers.emplace_back("");
}

void Image::readStbi(ifstream& f) {
    ThreadPool threadPool;

    static const stbi_io_callbacks callbacks = {
        // Read
        [](void* context, char* data, int size) {
            auto stream = reinterpret_cast<ifstream*>(context);
            stream->read(data, size);
            return (int)stream->gcount();
        },
        // Seek
        [](void* context, int size) {
            reinterpret_cast<ifstream*>(context)->seekg(size, ios_base::cur);
        },
        // EOF
        [](void* context) {
            return (int)!!(*reinterpret_cast<ifstream*>(context));
        },
    };

    void* data;
    int numChannels;
    Vector2i size;
    bool isHdr = stbi_is_hdr_from_callbacks(&callbacks, &f) != 0;
    f.seekg(0);

    if (isHdr) {
        data = stbi_loadf_from_callbacks(&callbacks, &f, &size.x(), &size.y(), &numChannels, 0);
    } else {
        data = stbi_load_from_callbacks(&callbacks, &f, &size.x(), &size.y(), &numChannels, 0);
    }

    if (!data) {
        throw invalid_argument{tfm::format("Could not load texture data from file %s", mPath)};
    }

    ScopeGuard dataGuard{[data] { stbi_image_free(data); }};

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

    for (auto& channel : channels) {
        string name = channel.name();
        if (matches(name, mChannelSelector, false)) {
            mChannels.emplace(move(name), move(channel));
        }
    }

    // STBI can not load layers, so all channels simply reside
    // within a topmost root layer.
    mLayers.emplace_back("");
}

void Image::readExr(ifstream& f) {
    ThreadPool threadPool;

    Imf::StdIFStream imfStream{f, mPath.str().c_str()};
    Imf::MultiPartInputFile multiPartFile{imfStream};
    int numParts = multiPartFile.parts();

    if (numParts <= 0) {
        throw invalid_argument{tfm::format("EXR image '%s' does not contain any parts.", mPath)};
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

shared_ptr<Image> tryLoadImage(path path, string channelSelector) {
    try {
        path = path.make_absolute();
    } catch (runtime_error e) {
        // If for some strange reason we can not obtain an absolute path, let's still
        // try to open the image at the given path just to make sure.
    }

    try {
        return make_shared<Image>(path, channelSelector);
    } catch (invalid_argument e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", path, channelSelector, e.what());
    } catch (runtime_error e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", path, channelSelector, e.what());
    } catch (Iex::BaseExc& e) {
        tfm::format(cerr, "Could not load image from %s:%s - %s\n", path, channelSelector, e.what());
    }

    return nullptr;
}

TEV_NAMESPACE_END
