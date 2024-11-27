// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ExrImageLoader.h>
#include <tev/ThreadPool.h>

#include <ImfChannelList.h>
#include <ImfChromaticities.h>
#include <ImfFrameBuffer.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfDoubleAttribute.h>
#include <ImfLineOrderAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfChannelListAttribute.h>
#include <ImfCompressionAttribute.h>
#include <Iex.h>

#include <istream>

#include <errno.h>

using namespace nanogui;
using namespace std;

namespace tev {

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

    uint64_t tellg() override {
        return streamoff(mStream.tellg());
    }

    void seekg(uint64_t pos) override {
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

AttributeNode createColorNode(const std::string& name, const Imath::V2f& value) {
    AttributeNode node; 
    node.name = name;
    node.type = "vec2f";
    std::ostringstream oss;
    oss << "(" << value[0] << ", " << value[1] << ")";
    node.value = oss.str();
    return node;
}
AttributeNode createColorNode(const std::string& name, const Imath::V2i& value) {
    AttributeNode node; 
    node.name = name;
    node.type = "vec2i";
    std::ostringstream oss;
    oss << "(" << value[0] << ", " << value[1] << ")";
    node.value = oss.str();
    return node;
}

AttributeNode getHeaderAttributes(const Imf::Header &header) {
    AttributeNode attributes;
    static std::map<Imf::PixelType, std::string> pixelTypeToStringMap{
        {Imf::PixelType::UINT,"UINT"},
        {Imf::PixelType::HALF,"HALF"},
        {Imf::PixelType::FLOAT,"FLOAT"}
    };
    for (auto attributeItr = header.begin(); attributeItr != header.end(); attributeItr++) {
        const Imf::Attribute *attr = &(attributeItr.attribute());
        AttributeNode node;
        node.name = std::string(attributeItr.name());
        node.type = std::string(attr->typeName());
        std::ostringstream oss;
        if (const Imf::StringAttribute *strAttr = dynamic_cast<const Imf::StringAttribute *>(attr)) {
            oss << strAttr->value();
        }
        else if (const Imf::IntAttribute *intAttr = dynamic_cast<const Imf::IntAttribute *>(attr)) {
            oss << intAttr->value();
        }
        else if (const Imf::FloatAttribute *floatAttr = dynamic_cast<const Imf::FloatAttribute *>(attr)) {
            oss << floatAttr->value();
        }
        else if (const Imf::DoubleAttribute *doubleAttr = dynamic_cast<const Imf::DoubleAttribute *>(attr)) {
            oss << doubleAttr->value();
        }
        else if (const Imf::V2fAttribute *v2fAttr = dynamic_cast<const Imf::V2fAttribute *>(attr)) {
            auto value = v2fAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ")";
        }
        else if (const Imf::V2dAttribute *v2dAttr = dynamic_cast<const Imf::V2dAttribute *>(attr)) {
            auto value = v2dAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ")";
        }
        else if (const Imf::V2iAttribute *v2iAttr = dynamic_cast<const Imf::V2iAttribute *>(attr)) {
            auto value = v2iAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ")";
        }
        else if (const Imf::V3fAttribute *v3fAttr = dynamic_cast<const Imf::V3fAttribute *>(attr)) {
            auto value = v3fAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ", " << value[2] << ")";
        }
        else if (const Imf::V3dAttribute *v3dAttr = dynamic_cast<const Imf::V3dAttribute *>(attr)) {
            auto value = v3dAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ", " << value[2] << ")";
        }
        else if (const Imf::V3iAttribute *v3iAttr = dynamic_cast<const Imf::V3iAttribute *>(attr)) {
            auto value = v3iAttr->value();
            oss << "(" << value[0] << ", " << value[1] << ", " << value[2] << ")";
        }
        else if (const Imf::Box2iAttribute *box2iAttr = dynamic_cast<const Imf::Box2iAttribute *>(attr)) {
            auto value = box2iAttr->value();
            AttributeNode minNode = createColorNode("min", value.min); 
            node.children.push_back(minNode);
            AttributeNode maxNode = createColorNode("max", value.max); 
            node.children.push_back(maxNode);
        }
        else if (const Imf::Box2fAttribute *box2fAttr = dynamic_cast<const Imf::Box2fAttribute *>(attr)) {
            auto value = box2fAttr->value();
            AttributeNode minNode = createColorNode("min", value.min); 
            node.children.push_back(minNode);
            AttributeNode maxNode = createColorNode("max", value.max); 
            node.children.push_back(maxNode);
        }
        else if (const Imf::M33fAttribute *m33fAttr = dynamic_cast<const Imf::M33fAttribute *>(attr)) {
            auto value = m33fAttr->value();
            oss << "("  
                << value[0] << ", " << value[1] << ", " << value[2] << ", "
                << value[3] << ", " << value[4] << ", " << value[5] << ", "
                << value[6] << ", " << value[7] << ", " << value[8] << ")";
        }
        else if (const Imf::M33dAttribute *m33dAttr = dynamic_cast<const Imf::M33dAttribute *>(attr)) {
            auto value = m33dAttr->value();
            oss << "("  
                << value[0] << ", " << value[1] << ", " << value[2] << ", "
                << value[3] << ", " << value[4] << ", " << value[5] << ", "
                << value[6] << ", " << value[7] << ", " << value[8] << ")";
        }
        else if (const Imf::M44fAttribute *m44fAttr = dynamic_cast<const Imf::M44fAttribute *>(attr)) {
            auto value = m44fAttr->value();
            oss << "("  
                << value[0] << ", " << value[1] << ", " << value[2] << ", " << value[3] << ", "
                << value[4] << ", " << value[5] << ", " << value[6] << ", " << value[7] << ", "
                << value[8] << ", " << value[9] << ", " << value[10] << ", " << value[11] << ", "
                << value[12] << ", " << value[13] << ", " << value[14] << ", " << value[15] << ")";
        }
        else if (const Imf::M44dAttribute *m44dAttr = dynamic_cast<const Imf::M44dAttribute *>(attr)) {
            auto value = m44dAttr->value();
            oss << "("  
                << value[0] << ", " << value[1] << ", " << value[2] << ", " << value[3] << ", "
                << value[4] << ", " << value[5] << ", " << value[6] << ", " << value[7] << ", "
                << value[8] << ", " << value[9] << ", " << value[10] << ", " << value[11] << ", "
                << value[12] << ", " << value[13] << ", " << value[14] << ", " << value[15] << ")";
        }
        else if (const Imf::EnvmapAttribute *envmapAttr = dynamic_cast<const Imf::EnvmapAttribute *>(attr)) {
            auto value = envmapAttr->value();
            oss << value;
        }
        else if (const Imf::CompressionAttribute *compressionAttr = dynamic_cast<const Imf::CompressionAttribute *>(attr)) {
            auto value = compressionAttr->value();
            oss << value;
        }
        else if (const Imf::LineOrderAttribute *lineOrderAttr = dynamic_cast<const Imf::LineOrderAttribute *>(attr)) {
            auto value = lineOrderAttr->value();
            oss << value;
        }
        else if (const Imf::KeyCodeAttribute *keycodeAttr = dynamic_cast<const Imf::KeyCodeAttribute *>(attr)) {
            auto value = keycodeAttr->value();
            oss << value.filmMfcCode() << ", " << value.filmType() << ", " << value.prefix() << ", "
                << value.count() << ", " << value.perfOffset() << ", " << value.perfsPerFrame() << ", "
                << value.perfsPerCount();
        }
        else if (const Imf::RationalAttribute *rationalAttr = dynamic_cast<const Imf::RationalAttribute *>(attr)) {
            auto value = rationalAttr->value();
            oss << value.n << ", " << value.d ;
        }
        else if (const Imf::ChromaticitiesAttribute *chromaticitiesAttr = dynamic_cast<const Imf::ChromaticitiesAttribute *>(attr)) {
            auto value = chromaticitiesAttr->value();

            AttributeNode redNode = createColorNode("red", value.red); 
            node.children.push_back(redNode);
            AttributeNode greenNode = createColorNode("green", value.green); 
            node.children.push_back(greenNode);
            AttributeNode blueNode = createColorNode("blue", value.blue); 
            node.children.push_back(blueNode);
            AttributeNode whiteNode = createColorNode("white", value.white); 
            node.children.push_back(whiteNode);
        }
        else if (const Imf::ChannelListAttribute *chlistAttr = dynamic_cast<const Imf::ChannelListAttribute *>(attr)) {
            auto chlist = chlistAttr->value();
            size_t cnt=0;
            for (auto myItr = chlist.begin(); myItr != chlist.end(); myItr++){
                Imf::Channel& channel = myItr.channel();
                AttributeNode chNode;
                chNode.name = myItr.name();
                chNode.type = pixelTypeToStringMap[channel.type];
                node.children.push_back(chNode);
                cnt+=1;
            }
            node.value = std::to_string(cnt);
        }
        else {
            oss << "unrecognized attribute : " << attributeItr.attribute().typeName();
        }
        // TODOS
        // Imf::StringVectorAttribute;
        // Imf::TimeCodeAttribute;
        // Imf::IDManifestAttribute;
        // Imf::DeepImageStateAttribute;
        node.value = oss.str();
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
        int widthSubsampled = width / mImfChannel.ySampling;

        auto data = reinterpret_cast<const T*>(mData.data());
        co_await ThreadPool::global().parallelForAsync<int>(0, channel.size().y(), [&, data](int y) {
            for (int x = 0; x < width; ++x) {
                channel.at({x, y}) = data[x / mImfChannel.xSampling + (y / mImfChannel.ySampling) * (size_t)widthSubsampled];
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
        throw invalid_argument{fmt::format("No channels match '{}'.", channelSelector)};
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
            data.attributes = getHeaderAttributes(part.header());

        Imath::Box2i dataWindow = part.header().dataWindow();
        Imath::Box2i displayWindow = part.header().displayWindow();

        // EXR's display- and data windows have inclusive upper ends while tev's upper ends are exclusive.
        // This allows easy conversion from window to size. Hence the +1.
        data.dataWindow =    {{dataWindow.min.x,    dataWindow.min.y   }, {dataWindow.max.x+1,    dataWindow.max.y+1   }};
        data.displayWindow = {{displayWindow.min.x, displayWindow.min.y}, {displayWindow.max.x+1, displayWindow.max.y+1}};

        if (!data.dataWindow.isValid()) {
            throw invalid_argument{fmt::format(
                "EXR image has invalid data window: [{},{}] - [{},{}]",
                data.dataWindow.min.x(), data.dataWindow.min.y(), data.dataWindow.max.x(), data.dataWindow.max.y()
            )};
        }

        if (!data.displayWindow.isValid()) {
            throw invalid_argument{fmt::format(
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

}
