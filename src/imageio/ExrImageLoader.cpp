// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ExrImageLoader.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStdIO.h>

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

bool ExrImageLoader::canLoadFile(ifstream& f) const {
    // Taken from http://www.openexr.com/ReadingAndWritingImageFiles.pdf
    char b[4];
    f.read(b, sizeof(b));

    bool result = !!f && f.gcount() == sizeof(b) && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;

    f.clear();
    f.seekg(0);
    return result;
}

ImageData ExrImageLoader::load(ifstream& f, const filesystem::path& path, const string& channelSelector) const {
    ImageData result;
    ThreadPool threadPool;

    Imf::StdIFStream imfStream{f, path.str().c_str()};
    Imf::MultiPartInputFile multiPartFile{imfStream};
    int numParts = multiPartFile.parts();

    if (numParts <= 0) {
        throw invalid_argument{"EXR image does not contain any parts."};
    }

    // Find the first part containing a channel that matches the given channelSubstr.
    int partIdx = 0;
    for (int i = 0; i < numParts; ++i) {
        Imf::InputPart part{multiPartFile, i};

        const Imf::ChannelList& imfChannels = part.header().channels();

        for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
            if (matchesFuzzy(c.name(), channelSelector)) {
                partIdx = i;
                goto l_foundPart;
            }
        }
    }
l_foundPart:

    Imf::InputPart file{multiPartFile, partIdx};
    Imath::Box2i dw = file.header().dataWindow();
    Vector2i size = {dw.max.x - dw.min.x + 1 , dw.max.y - dw.min.y + 1};

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"EXR image has zero pixels."};
    }

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
            // TODO: Switch to generic lambda once C++14 is used
            switch (mImfChannel.type) {
                case Imf::HALF: {
                    auto data = reinterpret_cast<const half*>(mData.data());
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    });
                    break;
                }

                case Imf::FLOAT: {
                    auto data = reinterpret_cast<const float*>(mData.data());
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    });
                    break;
                }

                case Imf::UINT: {
                    auto data = reinterpret_cast<const uint32_t*>(mData.data());
                    threadPool.parallelForNoWait<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    });
                    break;
                }

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

    using match_t = pair<size_t, Imf::ChannelList::ConstIterator>;
    vector<match_t> matches;
    for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
        size_t matchId;
        if (matchesFuzzy(c.name(), channelSelector, &matchId)) {
            matches.emplace_back(matchId, c);
            layerNames.insert(Channel::head(c.name()));
        }
    }

    // Sort matched channels by matched component of the selector, if one exists.
    if (!channelSelector.empty()) {
        sort(begin(matches), end(matches), [](const match_t& m1, const match_t& m2) { return m1.first < m2.first; });
    }

    for (const auto& match : matches) {
        const auto& c = match.second;
        rawChannels.emplace_back(c.name(), c.channel().type);
    }

    if (rawChannels.empty()) {
        throw invalid_argument{tfm::format("No channels match '%s'.", channelSelector)};
    }

    for (const string& layer : layerNames) {
        result.layers.emplace_back(layer);
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
        result.channels.emplace_back(Channel{rawChannel.name(), size});
    }

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].copyTo(result.channels[i], threadPool);
    }

    threadPool.waitUntilFinished();

    return result;
}

TEV_NAMESPACE_END
