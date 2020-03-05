// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ExrImageLoader.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <Iex.h>

#include <istream>

#include <errno.h>

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

class StdIStream: public Imf::IStream
{
public:
    StdIStream(istream& stream, const char fileName[])
    : Imf::IStream{fileName}, mStream{stream} { }

    bool read(char c[/*n*/], int n) override {
        if (!mStream)
            throw IEX_NAMESPACE::InputExc("Unexpected end of file.");

        clearError();
        mStream.read(c, n);
        return checkError(mStream, n);
    }

    Imf::Int64 tellg() override {
        return streamoff(mStream.tellg());
    }

    void seekg(Imf::Int64 pos) override {
        mStream.seekg(pos);
        checkError(mStream);
    }

    void clear() override {
        mStream.clear();
    }

private:
    // The following error-checking functions were copy&pasted from the OpenEXR source code
    static void clearError() {
        errno = 0;
    }

    static bool checkError(istream& is, streamsize expected = 0) {
        if (!is) {
            if (errno) {
                IEX_NAMESPACE::throwErrnoExc();
            }

            if (is.gcount() < expected) {
                THROW (IEX_NAMESPACE::InputExc, "Early end of file: read " << is.gcount() 
                    << " out of " << expected << " requested bytes.");
            }

            return false;
        }

        return true;
    }

    istream& mStream;
};

bool ExrImageLoader::canLoadFile(istream& iStream) const {
    // Taken from http://www.openexr.com/ReadingAndWritingImageFiles.pdf
    char b[4];
    iStream.read(b, sizeof(b));

    bool result = !!iStream && iStream.gcount() == sizeof(b) && b[0] == 0x76 && b[1] == 0x2f && b[2] == 0x31 && b[3] == 0x01;

    iStream.clear();
    iStream.seekg(0);
    return result;
}

ImageData ExrImageLoader::load(istream& iStream, const path& path, const string& channelSelector) const {
    ImageData result;
    ThreadPool threadPool;

    StdIStream stdIStream{iStream, path.str().c_str()};
    Imf::MultiPartInputFile multiPartFile{stdIStream};
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
                    auto data = reinterpret_cast<const ::half*>(mData.data());
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
                case Imf::HALF:  return sizeof(::half);
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
