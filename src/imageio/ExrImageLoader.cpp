/*
 * tev -- the EDR viewer
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
#include <ImfDeepImageStateAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfFloatVectorAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfIDManifestAttribute.h>
#include <ImfInputFile.h>
#include <ImfInputPart.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMultiPartInputFile.h>
#include <ImfOpaqueAttribute.h>
#include <ImfPreviewImageAttribute.h>
#include <ImfStandardAttributes.h>
#include <ImfStringVectorAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTimeCodeAttribute.h>

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

AttributeNode createVec2fNode(string_view name, const Imath::V2f& value) {
    AttributeNode node;
    node.name = name;
    node.type = "v2f";
    node.value = fmt::format("({}, {})", value[0], value[1]);
    return node;
}

AttributeNode createVec2iNode(string_view name, const Imath::V2i& value) {
    AttributeNode node;
    node.name = name;
    node.type = "v2i";
    node.value = fmt::format("({}, {})", value[0], value[1]);
    return node;
}

template <typename T> string toString(const Imath::Matrix33<T>& value) {
    ostringstream oss;
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

template <typename T> string toString(const Imath::Matrix44<T>& value) {
    ostringstream oss;
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

AttributeNode toAttributeNode(const Imf::Header& header) {
    AttributeNode result;
    result.name = "EXR header";

    AttributeNode& global = result.children.emplace_back();
    global.name = "Global";

    for (auto attributeItr = header.begin(); attributeItr != header.end(); attributeItr++) {
        const Imf::Attribute* attr = &(attributeItr.attribute());

        AttributeNode node;
        node.name = attributeItr.name();
        node.type = attr->typeName();

        if (const auto* strAttr = dynamic_cast<const Imf::StringAttribute*>(attr)) {
            node.value = strAttr->value();
        } else if (const auto* intAttr = dynamic_cast<const Imf::IntAttribute*>(attr)) {
            node.value = fmt::format("{}", intAttr->value());
        } else if (const auto* floatAttr = dynamic_cast<const Imf::FloatAttribute*>(attr)) {
            node.value = fmt::format("{}", floatAttr->value());
        } else if (const auto* doubleAttr = dynamic_cast<const Imf::DoubleAttribute*>(attr)) {
            node.value = fmt::format("{}", doubleAttr->value());
        } else if (const auto* v2fAttr = dynamic_cast<const Imf::V2fAttribute*>(attr)) {
            auto value = v2fAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const auto* v2dAttr = dynamic_cast<const Imf::V2dAttribute*>(attr)) {
            auto value = v2dAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const auto* v2iAttr = dynamic_cast<const Imf::V2iAttribute*>(attr)) {
            auto value = v2iAttr->value();
            node.value = fmt::format("({}, {})", value[0], value[1]);
        } else if (const auto* v3fAttr = dynamic_cast<const Imf::V3fAttribute*>(attr)) {
            auto value = v3fAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const auto* v3dAttr = dynamic_cast<const Imf::V3dAttribute*>(attr)) {
            auto value = v3dAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const auto* v3iAttr = dynamic_cast<const Imf::V3iAttribute*>(attr)) {
            auto value = v3iAttr->value();
            node.value = fmt::format("({}, {}, {})", value[0], value[1], value[2]);
        } else if (const auto* box2iAttr = dynamic_cast<const Imf::Box2iAttribute*>(attr)) {
            auto value = box2iAttr->value();
            AttributeNode minNode = createVec2iNode("min", value.min);
            node.children.push_back(minNode);
            AttributeNode maxNode = createVec2iNode("max", value.max);
            node.children.push_back(maxNode);
        } else if (const auto* box2fAttr = dynamic_cast<const Imf::Box2fAttribute*>(attr)) {
            auto value = box2fAttr->value();
            AttributeNode minNode = createVec2fNode("min", value.min);
            node.children.push_back(minNode);
            AttributeNode maxNode = createVec2fNode("max", value.max);
            node.children.push_back(maxNode);
        } else if (const auto* m33fAttr = dynamic_cast<const Imf::M33fAttribute*>(attr)) {
            node.value = toString(m33fAttr->value());
        } else if (const auto* m33dAttr = dynamic_cast<const Imf::M33dAttribute*>(attr)) {
            node.value = toString(m33dAttr->value());
        } else if (const auto* m44fAttr = dynamic_cast<const Imf::M44fAttribute*>(attr)) {
            node.value = toString(m44fAttr->value());
        } else if (const auto* m44dAttr = dynamic_cast<const Imf::M44dAttribute*>(attr)) {
            node.value = toString(m44dAttr->value());
        } else if (const auto* envmapAttr = dynamic_cast<const Imf::EnvmapAttribute*>(attr)) {
            switch (envmapAttr->value()) {
                case Imf::ENVMAP_LATLONG: node.value = "Latlong"; break;
                case Imf::ENVMAP_CUBE: node.value = "Cube"; break;
                default: node.value = "Unknown"; break;
            }
        } else if (const auto* compressionAttr = dynamic_cast<const Imf::CompressionAttribute*>(attr)) {
            switch (compressionAttr->value()) {
                case Imf::NO_COMPRESSION: node.value = "None"; break;
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
        } else if (const auto* lineOrderAttr = dynamic_cast<const Imf::LineOrderAttribute*>(attr)) {
            switch (lineOrderAttr->value()) {
                case Imf::INCREASING_Y: node.value = "Increasing Y"; break;
                case Imf::DECREASING_Y: node.value = "Decreasing Y"; break;
                case Imf::RANDOM_Y: node.value = "Random"; break;
                default: node.value = "Unknown"; break;
            }
        } else if (const auto* keycodeAttr = dynamic_cast<const Imf::KeyCodeAttribute*>(attr)) {
            auto value = keycodeAttr->value();
            node.children.push_back({"filmMfcCode", to_string(value.filmMfcCode()), "int", {}});
            node.children.push_back({"filmType", to_string(value.filmType()), "int", {}});
            node.children.push_back({"prefix", to_string(value.prefix()), "int", {}});
            node.children.push_back({"count", to_string(value.count()), "int", {}});
            node.children.push_back({"perfOffset", to_string(value.perfOffset()), "int", {}});
            node.children.push_back({"perfsPerFrame", to_string(value.perfsPerFrame()), "int", {}});
            node.children.push_back({"perfsPerCount", to_string(value.perfsPerCount()), "int", {}});
        } else if (const auto* rationalAttr = dynamic_cast<const Imf::RationalAttribute*>(attr)) {
            auto value = rationalAttr->value();
            node.value = fmt::format("{} / {}", value.n, value.d);
        } else if (const auto* chromaticitiesAttr = dynamic_cast<const Imf::ChromaticitiesAttribute*>(attr)) {
            auto value = chromaticitiesAttr->value();

            AttributeNode redNode = createVec2fNode("red", value.red);
            node.children.push_back(redNode);
            AttributeNode greenNode = createVec2fNode("green", value.green);
            node.children.push_back(greenNode);
            AttributeNode blueNode = createVec2fNode("blue", value.blue);
            node.children.push_back(blueNode);
            AttributeNode whiteNode = createVec2fNode("white", value.white);
            node.children.push_back(whiteNode);
        } else if (const auto* chlistAttr = dynamic_cast<const Imf::ChannelListAttribute*>(attr)) {
            auto toString = [](Imf::PixelType type) {
                switch (type) {
                    case Imf::UINT: return "uint";
                    case Imf::HALF: return "half";
                    case Imf::FLOAT: return "float";
                    default: return "Unknown";
                }
            };

            auto chlist = chlistAttr->value();
            for (auto chItr = chlist.begin(); chItr != chlist.end(); chItr++) {
                Imf::Channel& channel = chItr.channel();
                AttributeNode chNode;
                chNode.name = chItr.name();
                chNode.type = "channel";
                chNode.children.push_back({"type", toString(channel.type), "pixelType", {}});
                chNode.children.push_back({"xSampling", to_string(channel.xSampling), "int", {}});
                chNode.children.push_back({"ySampling", to_string(channel.ySampling), "int", {}});
                chNode.children.push_back({"pLinear", to_string(channel.pLinear), "bool", {}});

                node.children.emplace_back(chNode);
            }

            node.value = to_string(node.children.size());
        } else if (const auto* stringVectorAttr = dynamic_cast<const Imf::StringVectorAttribute*>(attr)) {
            node.value = join(stringVectorAttr->value(), ", ");
        } else if (const auto* floatVectorAttr = dynamic_cast<const Imf::FloatVectorAttribute*>(attr)) {
            node.value = join(floatVectorAttr->value(), ", ");
        } else if (const auto* tileDescAttr = dynamic_cast<const Imf::TileDescriptionAttribute*>(attr)) {
            auto modeToString = [](Imf::LevelMode mode) {
                switch (mode) {
                    case Imf::ONE_LEVEL: return "One level";
                    case Imf::MIPMAP_LEVELS: return "Mipmap levels";
                    case Imf::RIPMAP_LEVELS: return "Ripmap levels";
                    default: return "Unknown";
                }
            };

            auto roundingModeToString = [](Imf::LevelRoundingMode mode) {
                switch (mode) {
                    case Imf::ROUND_DOWN: return "Round down";
                    case Imf::ROUND_UP: return "Round up";
                    default: return "Unknown";
                }
            };

            auto value = tileDescAttr->value();
            node.children.push_back({"xSize", to_string(value.xSize), "int", {}});
            node.children.push_back({"ySize", to_string(value.ySize), "int", {}});
            node.children.push_back({"mode", modeToString(value.mode), "levelMode", {}});
            node.children.push_back({"roundingMode", roundingModeToString(value.roundingMode), "levelRoundingMode", {}});
        } else if (const auto* previewImageAttr = dynamic_cast<const Imf::PreviewImageAttribute*>(attr)) {
            node.children.push_back({"width", to_string(previewImageAttr->value().width()), "int", {}});
            node.children.push_back({"height", to_string(previewImageAttr->value().height()), "int", {}});
        } else if (const auto* deepImageStateAttr = dynamic_cast<const Imf::DeepImageStateAttribute*>(attr)) {
            auto toString = [](Imf::DeepImageState state) {
                switch (state) {
                    case Imf::DIS_MESSY: return "Messy";
                    case Imf::DIS_SORTED: return "Sorted";
                    case Imf::DIS_NON_OVERLAPPING: return "Non overlapping";
                    case Imf::DIS_TIDY: return "Tidy";
                    default: return "Unknown";
                }
            };
            node.value = toString(deepImageStateAttr->value());
        } else if (const auto* idManifestAttr = dynamic_cast<const Imf::IDManifestAttribute*>(attr)) {
            node.children.push_back({"compressedSize", to_string(idManifestAttr->value()._compressedDataSize), "int", {}});
            node.children.push_back({"uncompressedSize", to_string(idManifestAttr->value()._uncompressedDataSize), "size_t", {}});
        } else if (const auto* timeCodeAttr = dynamic_cast<const Imf::TimeCodeAttribute*>(attr)) {
            auto value = timeCodeAttr->value();
            node.value = fmt::format(
                "{:02}:{:02}:{:02}.{:03} {} {}",
                value.hours(),
                value.minutes(),
                value.seconds(),
                value.frame(),
                value.dropFrame() ? "DF" : "NDF",
                value.userData()
            );
        } else if (const auto* opaqueAttr = dynamic_cast<const Imf::OpaqueAttribute*>(attr)) {
            node.children.push_back({"size", to_string(opaqueAttr->dataSize()), "int", {}});
        } else {
            node.value = fmt::format("UNKNOWN: {}", attributeItr.attribute().typeName());
        }

        global.children.push_back(node);
    }

    return result;
}

// Helper class for dealing with the raw channels loaded from an exr file.
class RawChannel {
public:
    RawChannel(size_t partId, string_view name, string_view imfName, Imf::Channel imfChannel, const Vector2i& size) :
        mPartId{partId}, mName{name}, mImfName{imfName}, mImfChannel{imfChannel}, mSize{size} {}

    void resize() { mData = PixelBuffer::alloc((size_t)mSize.x() * mSize.y(), pixelFormat()); }

    void registerWith(Imf::FrameBuffer& frameBuffer, const Imath::Box2i& dw) {
        int width = dw.max.x - dw.min.x + 1;
        frameBuffer.insert(
            mImfName.c_str(),
            Imf::Slice(
                mImfChannel.type,
                reinterpret_cast<char*>(mData.dataBytes()) - (dw.min.x + dw.min.y * width) * bytesPerPixel(),
                bytesPerPixel(),
                bytesPerPixel() * (width / mImfChannel.xSampling),
                mImfChannel.xSampling,
                mImfChannel.ySampling,
                0
            )
        );
    }

    template <typename T> Task<void> copyToTyped(Channel& channel, int priority) const {
        const int width = channel.size().x();
        const int widthSubsampled = width / mImfChannel.ySampling;

        auto data = mData.data<T>();
        const auto view = channel.view<float>();
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            channel.size().y(),
            channel.numPixels(),
            [&, data](int y) {
                for (int x = 0; x < width; ++x) {
                    view[x, y] = data[(size_t)(x / mImfChannel.xSampling) + (size_t)(y / mImfChannel.ySampling) * widthSubsampled];
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

    string_view name() const { return mName; }

    const Vector2i& size() const { return mSize; }
    size_t numPixels() const { return (size_t)mSize.x() * mSize.y(); }

    EPixelFormat pixelFormat() const {
        switch (mImfChannel.type) {
            case Imf::HALF: return EPixelFormat::F16;
            case Imf::FLOAT: return EPixelFormat::F32;
            case Imf::UINT: return EPixelFormat::U32;
            default: throw runtime_error("Invalid pixel type encountered.");
        }
    }

    EPixelFormat desiredPixelFormat() const { return mImfChannel.type == Imf::HALF ? EPixelFormat::F16 : EPixelFormat::F32; }

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
    PixelBuffer mData;
};

Task<vector<ImageData>>
    ExrImageLoader::load(istream& iStream, const fs::path& path, string_view channelSelector, const ImageLoaderSettings&, int priority) const {
    try {
        if (!isExrImage(iStream)) {
            throw FormatNotSupported{"File is not an EXR image."};
        }

        StdIStream stdIStream{iStream, toString(path).c_str()};
        Imf::MultiPartInputFile multiPartFile{stdIStream};
        int numParts = multiPartFile.parts();

        if (numParts <= 0) {
            throw ImageLoadError{"EXR image does not contain any parts."};
        }

        vector<Imf::InputPart> parts;
        vector<Imf::FrameBuffer> frameBuffers;

        vector<RawChannel> rawChannels;

        // Load all parts that match the channel selector
        for (int partIdx = 0; partIdx < numParts; ++partIdx) {
            Imf::InputPart part{multiPartFile, partIdx};

            const Imf::ChannelList& imfChannels = part.header().channels();

            const auto channelName = [&](Imf::ChannelList::ConstIterator c) {
                return part.header().hasName() ? Channel::join(part.header().name(), c.name()) : c.name();
            };

            Imath::Box2i dataWindow = part.header().dataWindow();
            Vector2i size = {dataWindow.max.x - dataWindow.min.x + 1, dataWindow.max.y - dataWindow.min.y + 1};

            if (size.x() == 0 || size.y() == 0) {
                tlog::warning() << "EXR part '" << part.header().name() << "' has zero pixels.";
                continue;
            }

            bool matched = false;
            for (Imf::ChannelList::ConstIterator c = imfChannels.begin(); c != imfChannels.end(); ++c) {
                const string name = channelName(c);
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
            throw ImageLoadError{fmt::format("No channels match '{}'.", channelSelector)};
        }

        size_t totalNumPixels = 0;
        for (auto& rawChannel : rawChannels) {
            totalNumPixels += rawChannel.numPixels();
        }

        co_await ThreadPool::global().parallelForAsync(
            0, (int)rawChannels.size(), totalNumPixels, [&](int i) { rawChannels.at(i).resize(); }, priority
        );

        for (auto& rawChannel : rawChannels) {
            size_t partId = rawChannel.partId();
            rawChannel.registerWith(frameBuffers.at(partId), parts.at(partId).header().dataWindow());
        }

        vector<ImageData> result;

        // No need for a parallel for loop, because OpenEXR parallelizes internally
        for (size_t partIdx = 0; partIdx < parts.size(); ++partIdx) {
            auto& part = parts.at(partIdx);

            ImageData& data = result.emplace_back();
            try {
                data.attributes.emplace_back(toAttributeNode(part.header()));

                Imath::Box2i dataWindow = part.header().dataWindow();
                Imath::Box2i displayWindow = part.header().displayWindow();

                // EXR's display- and data windows have inclusive upper ends while tev's upper ends are exclusive. This allows easy
                // conversion from window to size. Hence the +1.
                data.dataWindow = {
                    {dataWindow.min.x,     dataWindow.min.y    },
                    {dataWindow.max.x + 1, dataWindow.max.y + 1}
                };
                data.displayWindow = {
                    {displayWindow.min.x,     displayWindow.min.y    },
                    {displayWindow.max.x + 1, displayWindow.max.y + 1}
                };

                if (!data.dataWindow.isValid()) {
                    throw ImageLoadError{
                        fmt::format("EXR image has invalid data window: min={}, max={}", data.dataWindow.min, data.dataWindow.max)
                    };
                }

                if (!data.displayWindow.isValid()) {
                    throw ImageLoadError{
                        fmt::format("EXR image has invalid display window: min={}, max={}", data.displayWindow.min, data.displayWindow.max)
                    };
                }

                part.setFrameBuffer(frameBuffers.at(partIdx));
                part.readPixels(dataWindow.min.y, dataWindow.max.y);

                data.hasPremultipliedAlpha = true;
                if (part.header().hasName()) {
                    data.partName = part.header().name();
                }

                if (Imf::hasWhiteLuminance(part.header())) {
                    const auto wl = Imf::whiteLuminance(part.header());
                    tlog::debug() << fmt::format("EXR part '{}' has white luminance {}", data.partName, wl);

                    data.hdrMetadata.bestGuessWhiteLevel = wl;
                }

                // OpenEXR, being linear, scene-referred, should not be falsified by white point adaptation. The test images provided at
                // https://github.com/AcademySoftwareFoundation/openexr-images/tree/main/Chromaticities also indicate that
                // AbsoluteColorimetric (no white point adaptation) is the intended behavior.
                data.renderingIntent = ERenderingIntent::AbsoluteColorimetric;

                // OpenEXR files may specify an "adoptedNeutral" white point that is meant to be rendered as neutral white. If this is the
                // case, we want white point adaptation after all and switch to RelativeColorimetric.
                optional<Vector2f> adoptedNeutral = nullopt;
                if (Imf::hasAdoptedNeutral(part.header())) {
                    const auto an = Imf::adoptedNeutral(part.header());

                    adoptedNeutral = Vector2f{an.x, an.y};
                    data.renderingIntent = ERenderingIntent::RelativeColorimetric;

                    tlog::debug() << fmt::format("EXR part '{}' has adopted neutral {}", data.partName, *adoptedNeutral);
                }

                chroma_t chroma = rec709Chroma(); // Assumption: EXR images are Rec. 709 unless specified otherwise
                if (Imf::hasChromaticities(part.header())) {
                    const auto c = Imf::chromaticities(part.header());
                    chroma = {
                        {{c.red.x, c.red.y}, {c.green.x, c.green.y}, {c.blue.x, c.blue.y}, {c.white.x, c.white.y}},
                    };

                    tlog::debug() << fmt::format("EXR part '{}' has chromaticities {}", data.partName, chroma);
                }

                data.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), data.renderingIntent, adoptedNeutral);

                data.nativeMetadata.chroma = chroma;
                data.nativeMetadata.transfer = ituth273::ETransfer::Linear;
            } catch (const Iex::BaseExc& e) {
                tlog::warning() << "Error reading EXR part " << partIdx << ": " << e.what();

                // Remove channels that belong to this part
                rawChannels.erase(
                    remove_if(rawChannels.begin(), rawChannels.end(), [partIdx](const RawChannel& ch) { return ch.partId() == partIdx; }),
                    rawChannels.end()
                );
            }
        }

        if (rawChannels.empty()) {
            throw ImageLoadError{"Could not read any EXR parts."};
        }

        vector<size_t> channelMapping;
        for (size_t i = 0; i < rawChannels.size(); ++i) {
            auto& rawChannel = rawChannels.at(i);
            auto& data = result.at(rawChannel.partId());
            channelMapping.emplace_back(data.channels.size());
            data.channels.emplace_back(Channel{rawChannel.name(), rawChannel.size(), EPixelFormat::F32, rawChannel.desiredPixelFormat()});
        }

        // Remove ImageData entries that have no channels. This can be malformed parts or parts that failed to load.
        result.erase(remove_if(result.begin(), result.end(), [](const ImageData& data) { return data.channels.empty(); }), result.end());

        vector<Task<void>> tasks;
        for (size_t i = 0; i < rawChannels.size(); ++i) {
            auto& rawChannel = rawChannels.at(i);
            tasks.emplace_back(rawChannel.copyTo(result.at(rawChannel.partId()).channels.at(channelMapping.at(i)), priority));
        }

        co_await awaitAll(tasks);
        co_return result;
    } catch (const Iex::BaseExc& e) {
        // Translate OpenEXR errors to our own error type
        throw ImageLoadError{e.what()};
    }
}
} // namespace tev
