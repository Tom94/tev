/*
 * tev -- the EXR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/ThreadPool.h>
#include <tev/imageio/Chroma.h>
#include <tev/imageio/ExrImageLoader.h>

#include <Iex.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStandardAttributes.h>

#include <istream>

#include <errno.h>

using namespace nanogui;
using namespace std;

namespace tev {

class StdIStream : public Imf::IStream {
public:
    StdIStream(istream& stream, const char fileName[]) : Imf::IStream{fileName}, mStream{stream} {}

    bool read(char c[/*n*/], int n) override {
        if (!mStream) {
            throw IEX_NAMESPACE::InputExc("Unexpected end of file.");
        }

        clearError();
        mStream.read(c, n);
        return checkError(mStream, n);
    }

    uint64_t tellg() override { return streamoff(mStream.tellg()); }

    void seekg(uint64_t pos) override {
        mStream.seekg(pos);
        checkError(mStream);
    }

    void clear() override { mStream.clear(); }

private:
    // The following error-checking functions were copy&pasted from the OpenEXR source code
    static void clearError() { errno = 0; }

    static bool checkError(istream& is, streamsize expected = 0) {
        if (!is) {
            if (errno) {
                IEX_NAMESPACE::throwErrnoExc();
            }

            if (is.gcount() < expected) {
                THROW(IEX_NAMESPACE::InputExc, "Early end of file: read " << is.gcount() << " out of " << expected << " requested bytes.");
            }

            return false;
        }

        return true;
    }

    istream& mStream;
};

static bool isExrImage(istream& iStream) {
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
    RawChannel(size_t partId, string name, string imfName, Imf::Channel imfChannel, const Vector2i& size) :
        mPartId{partId}, mName{name}, mImfName{imfName}, mImfChannel{imfChannel}, mSize{size} {}

    void resize() { mData.resize((size_t)mSize.x() * mSize.y() * bytesPerPixel()); }

    void registerWith(Imf::FrameBuffer& frameBuffer, const Imath::Box2i& dw) {
        int width = dw.max.x - dw.min.x + 1;
        frameBuffer.insert(
            mImfName.c_str(),
            Imf::Slice(
                mImfChannel.type,
                mData.data() - (dw.min.x + dw.min.y * width) * bytesPerPixel(),
                bytesPerPixel(),
                bytesPerPixel() * (width / mImfChannel.xSampling),
                mImfChannel.xSampling,
                mImfChannel.ySampling,
                0
            )
        );
    }

    template <typename T> Task<void> copyToTyped(Channel& channel, int priority) const {
        int width = channel.size().x();
        int widthSubsampled = width / mImfChannel.ySampling;

        auto data = reinterpret_cast<const T*>(mData.data());
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            channel.size().y(),
            [&, data](int y) {
                for (int x = 0; x < width; ++x) {
                    channel.at({x, y}
                    ) = data[(size_t)(x / mImfChannel.xSampling) + (size_t)(y / mImfChannel.ySampling) * (size_t)widthSubsampled];
                }
            },
            priority
        );
    }

    Task<void> copyTo(Channel& channel, int priority) const {
        switch (mImfChannel.type) {
            case Imf::HALF: co_await copyToTyped<::half>(channel, priority); break;
            case Imf::FLOAT: co_await copyToTyped<float>(channel, priority); break;
            case Imf::UINT: co_await copyToTyped<uint32_t>(channel, priority); break;
            default: throw runtime_error("Invalid pixel type encountered.");
        }
    }

    size_t partId() const { return mPartId; }

    const string& name() const { return mName; }

    const Vector2i& size() const { return mSize; }

private:
    int bytesPerPixel() const {
        switch (mImfChannel.type) {
            case Imf::HALF: return sizeof(::half);
            case Imf::FLOAT: return sizeof(float);
            case Imf::UINT: return sizeof(uint32_t);
            default: throw runtime_error("Invalid pixel type encountered.");
        }
    }

    size_t mPartId;
    string mName;
    string mImfName;
    Imf::Channel mImfChannel;
    Vector2i mSize;
    vector<char> mData;
};

Task<vector<ImageData>> ExrImageLoader::load(istream& iStream, const fs::path& path, const string& channelSelector, int priority, bool) const {
    try {
        if (!isExrImage(iStream)) {
            throw FormatNotSupported{"File is not an EXR image."};
        }

        StdIStream stdIStream{iStream, toString(path).c_str()};
        Imf::MultiPartInputFile multiPartFile{stdIStream};
        int numParts = multiPartFile.parts();

        if (numParts <= 0) {
            throw LoadError{"EXR image does not contain any parts."};
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
            Vector2i size = {dataWindow.max.x - dataWindow.min.x + 1, dataWindow.max.y - dataWindow.min.y + 1};

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
            throw LoadError{fmt::format("No channels match '{}'.", channelSelector)};
        }

        co_await ThreadPool::global().parallelForAsync(0, (int)rawChannels.size(), [&](int i) { rawChannels.at(i).resize(); }, priority);

        for (auto& rawChannel : rawChannels) {
            size_t partId = rawChannel.partId();
            rawChannel.registerWith(frameBuffers.at(partId), parts.at(partId).header().dataWindow());
        }

        vector<ImageData> result;

        // No need for a parallel for loop, because OpenEXR parallelizes internally
        for (size_t partIdx = 0; partIdx < parts.size(); ++partIdx) {
            auto& part = parts.at(partIdx);

            result.emplace_back();
            ImageData& data = result.back();

            Imath::Box2i dataWindow = part.header().dataWindow();
            Imath::Box2i displayWindow = part.header().displayWindow();

            // EXR's display- and data windows have inclusive upper ends while tev's upper ends are exclusive. This allows easy conversion
            // from window to size. Hence the +1.
            data.dataWindow = {
                {dataWindow.min.x,     dataWindow.min.y    },
                {dataWindow.max.x + 1, dataWindow.max.y + 1}
            };
            data.displayWindow = {
                {displayWindow.min.x,     displayWindow.min.y    },
                {displayWindow.max.x + 1, displayWindow.max.y + 1}
            };

            if (!data.dataWindow.isValid()) {
                throw LoadError{fmt::format(
                    "EXR image has invalid data window: [{},{}] - [{},{}]",
                    data.dataWindow.min.x(),
                    data.dataWindow.min.y(),
                    data.dataWindow.max.x(),
                    data.dataWindow.max.y()
                )};
            }

            if (!data.displayWindow.isValid()) {
                throw LoadError{fmt::format(
                    "EXR image has invalid display window: [{},{}] - [{},{}]",
                    data.displayWindow.min.x(),
                    data.displayWindow.min.y(),
                    data.displayWindow.max.x(),
                    data.displayWindow.max.y()
                )};
            }

            part.setFrameBuffer(frameBuffers.at(partIdx));
            part.readPixels(dataWindow.min.y, dataWindow.max.y);

            data.hasPremultipliedAlpha = true;
            if (part.header().hasName()) {
                data.partName = part.header().name();
            }

            if (Imf::hasChromaticities(part.header())) {
                auto chroma = Imf::chromaticities(part.header());
                data.toRec709 = convertChromaToRec709({
                    {{chroma.red.x, chroma.red.y},
                     {chroma.green.x, chroma.green.y},
                     {chroma.blue.x, chroma.blue.y},
                     {chroma.white.x, chroma.white.y}}
                });
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
    } catch (const Iex::BaseExc& e) {
        // Translate OpenEXR errors to our own error type
        throw LoadError{e.what()};
    }
}
} // namespace tev
