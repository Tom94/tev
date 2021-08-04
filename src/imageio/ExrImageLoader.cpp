// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ExrImageLoader.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStandardAttributes.h>
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

Task<std::tuple<ImageData, bool>> ExrImageLoader::load(istream& iStream, const path& path, const string& channelSelector, int priority) const {
    ImageData result;

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

        std::future<void> copyTo(Channel& channel, int priority) const {
            switch (mImfChannel.type) {
                case Imf::HALF: {
                    auto data = reinterpret_cast<const ::half*>(mData.data());
                    return gThreadPool->parallelForAsync<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    }, priority);
                }

                case Imf::FLOAT: {
                    auto data = reinterpret_cast<const float*>(mData.data());
                    return gThreadPool->parallelForAsync<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    }, priority);
                }

                case Imf::UINT: {
                    auto data = reinterpret_cast<const uint32_t*>(mData.data());
                    return gThreadPool->parallelForAsync<DenseIndex>(0, channel.count(), [&, data](DenseIndex i) {
                        channel.at(i) = data[i];
                    }, priority);
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
        rawChannels.emplace_back(c.name(), c.channel());
    }

    if (rawChannels.empty()) {
        throw invalid_argument{tfm::format("No channels match '%s'.", channelSelector)};
    }

    for (const string& layer : layerNames) {
        result.layers.emplace_back(layer);
    }

    gThreadPool->parallelFor(0, (int)rawChannels.size(), [&](int i) {
        rawChannels[i].resize((DenseIndex)size.x() * size.y());
    }, priority);

    for (size_t i = 0; i < rawChannels.size(); ++i) {
        rawChannels[i].registerWith(frameBuffer, dw);
    }

    file.setFrameBuffer(frameBuffer);
    file.readPixels(dw.min.y, dw.max.y);

    for (const auto& rawChannel : rawChannels) {
        result.channels.emplace_back(Channel{rawChannel.name(), size});
    }

    vector<future<void>> futures;
    for (size_t i = 0; i < rawChannels.size(); ++i) {
        futures.emplace_back(rawChannels[i].copyTo(result.channels[i], priority));
    }
    waitAll(futures);

    hasPremultipliedAlpha = true;

    // equality comparison for Imf::Chromaticities instances
    auto chromaEq = [](const Imf::Chromaticities& a, const Imf::Chromaticities& b) {
        return
            (a.red  - b.red).length2() + (a.green - b.green).length2() +
            (a.blue - b.blue).length2() + (a.white - b.white).length2() < 1e-6f;
    };

    Imf::Chromaticities rec709; // default rec709 (sRGB) primaries

    // Check if there is a chromaticity header entry and if so,
    // expose it to the image data for later conversion to sRGB/Rec709.
    Imf::Chromaticities chroma;
    if (Imf::hasChromaticities(file.header())) {
        chroma = Imf::chromaticities(file.header());
    }

    if (!chromaEq(chroma, rec709)) {
        Imath::M44f M = Imf::RGBtoXYZ(chroma, 1) * Imf::XYZtoRGB(rec709, 1);
        for (int m = 0; m < 4; ++m) {
            for (int n = 0; n < 4; ++n) {
                result.toRec709.m[m][n] = M.x[m][n];
            }
        }
    }

    co_return {result, true};
}

TEV_NAMESPACE_END
