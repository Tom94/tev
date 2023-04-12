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

using namespace nanogui;
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

// Helper class for dealing with the raw channels loaded from an exr file.
class RawChannel {
public:
    RawChannel(size_t partId, string name, string imfName, Imf::Channel imfChannel, const Vector2i& size)
    : mPartId{partId}, mName{name}, mImfName{imfName}, mImfChannel{imfChannel}, mSize{size} {}

    void resize() {
        mData.resize((size_t)mSize.x() * mSize.y() * bytesPerPixel());
    }

    void registerWith(Imf::FrameBuffer& frameBuffer, const Imath::Box2i& dw) {
        int width = dw.max.x - dw.min.x + 1;
        frameBuffer.insert(mImfName.c_str(), Imf::Slice(
            mImfChannel.type,
            mData.data() - (dw.min.x + dw.min.y * width) * bytesPerPixel(),
            bytesPerPixel(), bytesPerPixel() * (width/mImfChannel.xSampling),
            mImfChannel.xSampling, mImfChannel.ySampling, 0
        ));
    }

    template <typename T>
    Task<void> copyToTyped(Channel& channel, int priority) const {
        int width = channel.size().x();
        int widthSubsampled = width/mImfChannel.ySampling;

        auto data = reinterpret_cast<const T*>(mData.data());
        co_await ThreadPool::global().parallelForAsync<int>(0, channel.size().y(), [&, data](int y) {
            for (int x = 0; x < width; ++x) {
                channel.at({x, y}) = data[x/mImfChannel.xSampling + (y/mImfChannel.ySampling) * widthSubsampled];
            }
        }, priority);
    }

    Task<void> copyTo(Channel& channel, int priority) const {
        switch (mImfChannel.type) {
            case Imf::HALF:
                co_await copyToTyped<::half>(channel, priority); break;
            case Imf::FLOAT:
                co_await copyToTyped<float>(channel, priority); break;
            case Imf::UINT:
                co_await copyToTyped<uint32_t>(channel, priority); break;
            default:
                throw runtime_error("Invalid pixel type encountered.");
        }
    }

    size_t partId() const {
        return mPartId;
    }

    const string& name() const {
        return mName;
    }

    const Vector2i& size() const {
        return mSize;
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

    size_t mPartId;
    string mName;
    string mImfName;
    Imf::Channel mImfChannel;
    Vector2i mSize;
    vector<char> mData;
};

Task<vector<ImageData>> ExrImageLoader::load(istream& iStream, const fs::path& path, const string& channelSelector, int priority) const {
    vector<ImageData> result;

    StdIStream stdIStream{iStream, toString(path).c_str()};
    Imf::MultiPartInputFile multiPartFile{stdIStream};
    int numParts = multiPartFile.parts();

    if (numParts <= 0) {
        throw invalid_argument{"EXR image does not contain any parts."};
    }

    vector<Imf::InputPart> parts;
    vector<Imf::FrameBuffer> frameBuffers;

    vector<RawChannel> rawChannels;

    // Load all parts that match the channel selector
    for (int partIdx = 0; partIdx < numParts; ++partIdx) {
        Imf::InputPart part{multiPartFile, partIdx};

        const Imf::ChannelList& imfChannels = part.header().channels();

        auto channelName = [&](Imf::ChannelList::ConstIterator c) {
            string name = c.name();
            if (part.header().hasName()) {
                name = part.header().name() + "."s + name;
            }
            return name;
        };

        Imath::Box2i dataWindow = part.header().dataWindow();
        Vector2i size = {dataWindow.max.x - dataWindow.min.x + 1 , dataWindow.max.y - dataWindow.min.y + 1};

        if (size.x() == 0 || size.y() == 0) {
            tlog::warning() << "EXR part '" << part.header().name() << "' has zero pixels.";
            continue;
        }

        bool matched = false;
        for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
            string name = channelName(c);
            if (matchesFuzzy(name, channelSelector)) {
                rawChannels.emplace_back(parts.size(), name, c.name(), c.channel(), size);
                matched = true;
            }
        }

        if (!matched) {
            continue;
        }

        parts.emplace_back(part);
        frameBuffers.emplace_back();
    }

    if (rawChannels.empty()) {
        throw invalid_argument{format("No channels match '{}'.", channelSelector)};
    }

    co_await ThreadPool::global().parallelForAsync(0, (int)rawChannels.size(), [&](int i) {
        rawChannels.at(i).resize();
    }, priority);

    for (auto& rawChannel : rawChannels) {
        size_t partId = rawChannel.partId();
        rawChannel.registerWith(frameBuffers.at(partId), parts.at(partId).header().dataWindow());
    }

    // No need for a parallel for loop, because OpenEXR parallelizes internally
    for (size_t partIdx = 0; partIdx < parts.size(); ++partIdx) {
        auto& part = parts.at(partIdx);

        result.emplace_back();
        ImageData& data = result.back();

        Imath::Box2i dataWindow = part.header().dataWindow();
        Imath::Box2i displayWindow = part.header().displayWindow();

        // EXR's display- and data windows have inclusive upper ends while tev's upper ends are exclusive.
        // This allows easy conversion from window to size. Hence the +1.
        data.dataWindow =    {{dataWindow.min.x,    dataWindow.min.y   }, {dataWindow.max.x+1,    dataWindow.max.y+1   }};
        data.displayWindow = {{displayWindow.min.x, displayWindow.min.y}, {displayWindow.max.x+1, displayWindow.max.y+1}};

        if (!data.dataWindow.isValid()) {
            throw invalid_argument{format(
                "EXR image has invalid data window: [{},{}] - [{},{}]",
                data.dataWindow.min.x(), data.dataWindow.min.y(), data.dataWindow.max.x(), data.dataWindow.max.y()
            )};
        }

        if (!data.displayWindow.isValid()) {
            throw invalid_argument{format(
                "EXR image has invalid display window: [{},{}] - [{},{}]",
                data.displayWindow.min.x(), data.displayWindow.min.y(), data.displayWindow.max.x(), data.displayWindow.max.y()
            )};
        }

        part.setFrameBuffer(frameBuffers.at(partIdx));
        part.readPixels(dataWindow.min.y, dataWindow.max.y);

        data.hasPremultipliedAlpha = true;
        if (part.header().hasName()) {
            data.partName = part.header().name();
        }

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
        if (Imf::hasChromaticities(part.header())) {
            chroma = Imf::chromaticities(part.header());
        }

        if (!chromaEq(chroma, rec709)) {
            Imath::M44f M = Imf::RGBtoXYZ(chroma, 1) * Imf::XYZtoRGB(rec709, 1);
            for (int m = 0; m < 4; ++m) {
                for (int n = 0; n < 4; ++n) {
                    data.toRec709.m[m][n] = M.x[m][n];
                }
            }
        }
    }

    vector<size_t> channelMapping;
    for (size_t i = 0; i < rawChannels.size(); ++i) {
        auto& rawChannel = rawChannels.at(i);
        auto& data = result.at(rawChannel.partId());
        channelMapping.emplace_back(data.channels.size());
        data.channels.emplace_back(Channel{rawChannel.name(), rawChannel.size()});
    }

    vector<Task<void>> tasks;
    for (size_t i = 0; i < rawChannels.size(); ++i) {
        auto& rawChannel = rawChannels.at(i);
        tasks.emplace_back(rawChannel.copyTo(result.at(rawChannel.partId()).channels.at(channelMapping.at(i)), priority));
    }

    for (auto& task : tasks) {
        co_await task;
    }

    co_return result;
}

TEV_NAMESPACE_END
