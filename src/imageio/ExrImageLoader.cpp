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
#include <tev/imageio/Colors.h>
#include <tev/imageio/ExrImageLoader.h>

#include <Iex.h>
#include <ImfChannelList.h>
#include <ImfChannelListAttribute.h>
#include <ImfCompressionAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfTileDescriptionAttribute.h>

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

AttributeNode createColorNode(const std::string& name, const Imath::V2f& value) {
    AttributeNode node;
    node.name = name;
    node.type = "vec2f";
    node.value = fmt::format("({}, {})", value[0], value[1]);
    return node;
}

AttributeNode createColorNode(const std::string& name, const Imath::V2i& value) {
    AttributeNode node;
    node.name = name;
    node.type = "vec2i";
    node.value = fmt::format("({}, {})", value[0], value[1]);
    return node;
}

template <typename T>
std::string toString(const Imath::Matrix33<T>& value) {
    std::ostringstream oss;
    oss << "([";
    for (uint32_t i = 0; i < 3; ++i) {
        if (i > 0) {
            oss << "], [";
        }
        for (uint32_t j = 0; j < 3; ++j) {
            if (j > 0) {
                oss << ", ";
            }
            oss << value[i][j];
        }
    }
    oss << "])";
    return oss.str();
}

template <typename T>
std::string toString(const Imath::Matrix44<T>& value) {
    std::ostringstream oss;
    oss << "([";
    for (uint32_t i = 0; i < 4; ++i) {
        if (i > 0) {
            oss << "], [";
        }
        for (uint32_t j = 0; j < 4; ++j) {
            if (j > 0) {
                oss << ", ";
            }
            oss << value[i][j];
        }
    }
    oss << "])";
    return oss.str();
}

AttributeNode getHeaderAttributes(const Imf::Header& header) {
    AttributeNode attributes;
    static std::map<Imf::PixelType, std::string> pixelTypeToStringMap{
        {Imf::PixelType::UINT,  "UINT" },
        {Imf::PixelType::HALF,  "HALF" },
        {Imf::PixelType::FLOAT, "FLOAT"}
    };

    for (auto attributeItr = header.begin(); attributeItr != header.end(); attributeItr++) {
        const Imf::Attribute* attr = &(attributeItr.attribute());

        AttributeNode node;
        node.name = std::string(attributeItr.name());
        node.type = std::string(attr->typeName());

        if (const Imf::StringAttribute* strAttr = dynamic_cast<const Imf::StringAttribute*>(attr)) {
            node.value = strAttr->value();
        } else if (const Imf::IntAttribute* intAttr = dynamic_cast<const Imf::IntAttribute*>(attr)) {
            node.value = fmt::format("{}", intAttr->value());
        } else if (const Imf::FloatAttribute* floatAttr = dynamic_cast<const Imf::FloatAttribute*>(attr)) {
            node.value = fmt::format("{}", floatAttr->value());
        } else if (const Imf::DoubleAttribute* doubleAttr = dynamic_cast<const Imf::DoubleAttribute*>(attr)) {
            node.value = fmt::format("{}", doubleAttr->value());
        } else if (const Imf::V2fAttribute* v2fAttr = dynamic_cast<const Imf::V2fAttribute*>(attr)) {
            auto value = v2fAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const Imf::V2dAttribute* v2dAttr = dynamic_cast<const Imf::V2dAttribute*>(attr)) {
            auto value = v2dAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const Imf::V2iAttribute* v2iAttr = dynamic_cast<const Imf::V2iAttribute*>(attr)) {
            auto value = v2iAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const Imf::V3fAttribute* v3fAttr = dynamic_cast<const Imf::V3fAttribute*>(attr)) {
            auto value = v3fAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const Imf::V3dAttribute* v3dAttr = dynamic_cast<const Imf::V3dAttribute*>(attr)) {
            auto value = v3dAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const Imf::V3iAttribute* v3iAttr = dynamic_cast<const Imf::V3iAttribute*>(attr)) {
            auto value = v3iAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const Imf::Box2iAttribute* box2iAttr = dynamic_cast<const Imf::Box2iAttribute*>(attr)) {
            auto value = box2iAttr->value();
            AttributeNode minNode = createColorNode("min", value.min);
            node.children.push_back(minNode);
            AttributeNode maxNode = createColorNode("max", value.max);
            node.children.push_back(maxNode);
        } else if (const Imf::Box2fAttribute* box2fAttr = dynamic_cast<const Imf::Box2fAttribute*>(attr)) {
            auto value = box2fAttr->value();
            AttributeNode minNode = createColorNode("min", value.min);
            node.children.push_back(minNode);
            AttributeNode maxNode = createColorNode("max", value.max);
            node.children.push_back(maxNode);
        } else if (const Imf::M33fAttribute* m33fAttr = dynamic_cast<const Imf::M33fAttribute*>(attr)) {
            node.value = toString(m33fAttr->value());
        } else if (const Imf::M33dAttribute* m33dAttr = dynamic_cast<const Imf::M33dAttribute*>(attr)) {
            node.value = toString(m33dAttr->value());
        } else if (const Imf::M44fAttribute* m44fAttr = dynamic_cast<const Imf::M44fAttribute*>(attr)) {
            node.value = toString(m44fAttr->value());
        } else if (const Imf::M44dAttribute* m44dAttr = dynamic_cast<const Imf::M44dAttribute*>(attr)) {
            node.value = toString(m44dAttr->value());
        } else if (const Imf::EnvmapAttribute* envmapAttr = dynamic_cast<const Imf::EnvmapAttribute*>(attr)) {
            switch (envmapAttr->value()) {
                case Imf::ENVMAP_LATLONG: node.value = "Latlong"; break;
                case Imf::ENVMAP_CUBE: node.value = "Cube"; break;
                default: node.value = "Unknown"; break;
            }
        } else if (const Imf::CompressionAttribute* compressionAttr = dynamic_cast<const Imf::CompressionAttribute*>(attr)) {
            switch (compressionAttr->value()) {
                case Imf::NO_COMPRESSION: node.value = "NO"; break;
                case Imf::RLE_COMPRESSION: node.value = "RLE"; break;
                case Imf::ZIPS_COMPRESSION: node.value = "ZIPS"; break;
                case Imf::ZIP_COMPRESSION: node.value = "ZIP"; break;
                case Imf::PIZ_COMPRESSION: node.value = "PIZ"; break;
                case Imf::B44_COMPRESSION: node.value = "B44"; break;
                case Imf::B44A_COMPRESSION: node.value = "B44A"; break;
                case Imf::DWAA_COMPRESSION: node.value = "DWAA"; break;
                case Imf::DWAB_COMPRESSION: node.value = "DWAB"; break;
                default: node.value = "Unknown"; break;
            }
        } else if (const Imf::LineOrderAttribute* lineOrderAttr = dynamic_cast<const Imf::LineOrderAttribute*>(attr)) {
            switch (lineOrderAttr->value()) {
                case Imf::INCREASING_Y: node.value = "Increasing Y"; break;
                case Imf::DECREASING_Y: node.value = "Decreasing Y"; break;
                case Imf::RANDOM_Y: node.value = "Random"; break;
                default: node.value = "Unknown"; break;
            }
        } else if (const Imf::KeyCodeAttribute* keycodeAttr = dynamic_cast<const Imf::KeyCodeAttribute*>(attr)) {
            auto value = keycodeAttr->value();
            node.value = fmt::format(
                "{}, {}, {}, {}, {}, {}, {}",
                value.filmMfcCode(),
                value.filmType(),
                value.prefix(),
                value.count(),
                value.perfOffset(),
                value.perfsPerFrame(),
                value.perfsPerCount()
            );
        } else if (const Imf::RationalAttribute* rationalAttr = dynamic_cast<const Imf::RationalAttribute*>(attr)) {
            auto value = rationalAttr->value();
            node.value = fmt::format("{}, {}", value.n, value.d);
        } else if (const Imf::ChromaticitiesAttribute* chromaticitiesAttr = dynamic_cast<const Imf::ChromaticitiesAttribute*>(attr)) {
            auto value = chromaticitiesAttr->value();

            AttributeNode redNode = createColorNode("red", value.red);
            node.children.push_back(redNode);
            AttributeNode greenNode = createColorNode("green", value.green);
            node.children.push_back(greenNode);
            AttributeNode blueNode = createColorNode("blue", value.blue);
            node.children.push_back(blueNode);
            AttributeNode whiteNode = createColorNode("white", value.white);
            node.children.push_back(whiteNode);
        } else if (const Imf::ChannelListAttribute* chlistAttr = dynamic_cast<const Imf::ChannelListAttribute*>(attr)) {
            auto chlist = chlistAttr->value();
            size_t cnt = 0;
            for (auto myItr = chlist.begin(); myItr != chlist.end(); myItr++) {
                Imf::Channel& channel = myItr.channel();
                AttributeNode chNode;
                chNode.name = myItr.name();
                chNode.type = pixelTypeToStringMap[channel.type];
                node.children.push_back(chNode);
                cnt += 1;
            }
            node.value = std::to_string(cnt);
        } else {
            node.value = fmt::format("UNKNOWN: {}", attributeItr.attribute().typeName());
        }

        // TODOS
        // Imf::StringVectorAttribute;
        // Imf::TimeCodeAttribute;
        // Imf::IDManifestAttribute;
        // Imf::DeepImageStateAttribute;
        attributes.children.push_back(node);
    }

    // {
    //     std::ostringstream oss;
    //     auto ch = header.channels();
    //     oss << header.channels();
    //     attributes["compression"] = oss.str();
    // }

    return attributes;
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

            data.attributes = getHeaderAttributes(part.header());

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
                data.toRec709 = chromaToRec709Matrix({
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
