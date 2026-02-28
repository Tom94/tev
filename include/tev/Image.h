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

#pragma once

#include <tev/Box.h>
#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/VectorGraphics.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/GainMap.h>

#include <nanogui/texture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tev {

struct ImageLoaderSettings;

static constexpr float DEFAULT_IMAGE_WHITE_LEVEL = 80.0f;

struct AttributeNode {
    std::string name;
    std::string value;
    std::string type;
    std::vector<AttributeNode> children;
};

struct HdrMetadata {
    float maxCLL = 0.0f;
    float maxFALL = 0.0f;

    float masteringMinLum = 0.0f;
    float masteringMaxLum = 0.0f;
    chroma_t masteringChroma = zeroChroma();

    float bestGuessWhiteLevel = DEFAULT_IMAGE_WHITE_LEVEL;

    AttributeNode toAttributes() const;
};

struct NativeImageMetadata {
    std::optional<chroma_t> chroma = std::nullopt;
    std::optional<ituth273::ETransfer> transfer = std::nullopt;
    std::optional<float> gamma = std::nullopt; // Only used if transfer is ituth273::ETransfer::GenericGamma
};

struct ImageData {
    ImageData() = default;
    ImageData(const ImageData&) = delete;
    ImageData(ImageData&&) = default;
    ImageData& operator=(const ImageData&) = delete;
    ImageData& operator=(ImageData&&) = default;

    std::vector<Channel> channels;
    std::vector<std::string> layers;
    nanogui::Matrix3f toRec709 = nanogui::Matrix3f{1.0f}; // Identity by default
    bool hasPremultipliedAlpha = false;
    EOrientation orientation = EOrientation::TopLeft;
    std::vector<AttributeNode> attributes;

    HdrMetadata hdrMetadata;
    NativeImageMetadata nativeMetadata; // Information about the image's original color space, etc.

    // tev only really supports two rendering intents: relative and absolute colorimetric. The reason being that the other rendering intents
    // (perceptual and saturation) are subjective while tev, as an image analysis tool, should be as objective as possible. The difference
    // between relative and absolute colorimetric is that the former performs white point adaptation while the latter does not. Which of the
    // two is more appropriate/correct depends on what the image data represents:
    // - If the image data is display-referred (i.e. already adapted to a specific viewing condition), relative colorimetric is appropriate
    //   because the user wants the image to remain adapted to new viewing conditions. Examples are typical SDR formats (JPEG, PNG, etc.),
    //   as well as most HDR formats (e.g. extended PNG, HDR10 data). In tev, even camera RAW images fall under this category, because the
    //   underlying loaders (e.g. libraw) already perform color adaptation to D65 viewing conditions. Technically, RAW files could be left
    //   in scene-referred space, but that would break convention with other RAW viewers.
    // - If the image data is scene-referred (i.e. representing real-world photon counts) absolute colorimetric is appropriate because the
    //   user wants to analyze the scene-referred colors without any adaptation. Examples are EXR and PFM files that often come out of
    //   renderers or are used in visual effects pipelines.
    // NOTE: scene- vs. display-referred is orthogonal to the question of absolute vs. relative brightness. Some formats like HDR10 are
    // display referred (mastered to a specific viewing condition) while representing absolute brightness levels (in cd/m²). Other
    // display-referred formats describe relative brightness (e.g. SDR sRGB). Yet others, like OpenEXR files from renderers, are scene
    // referred while representing relative brightness levels only unless tagged with non-standard metadata.
    ERenderingIntent renderingIntent = ERenderingIntent::RelativeColorimetric;

    Box2i dataWindow;
    Box2i displayWindow;

    std::string partName;

    void readMetadataFromIcc(const ColorProfile& profile);
    void readMetadataFromCicp(const ColorProfile::CICP& cicp);

    nanogui::Vector2i size() const { return dataWindow.size(); }

    nanogui::Vector2i displaySize() const { return displayWindow.size(); }

    size_t numPixels() const { return channels.front().numPixels(); }

    std::vector<std::string> channelsInLayer(std::string_view layerName) const;

    Task<void> applyColorConversion(const nanogui::Matrix3f& mat, int priority);
    Task<void> convertToRec709(int priority);

    Task<void> convertYCbCrToRgb(int priority);

    Task<void> matchColorsAndSizeOf(const ImageData& other, int priority);

    Task<void> deriveWhiteLevelFromMetadata(int priority);
    Task<void> convertToDesiredPixelFormat(int priority);

    void alphaOperation(const std::function<void(Channel&, const Channel&)>& func);

    Task<void> multiplyAlpha(int priority);
    Task<void> unmultiplyAlpha(int priority);

    Task<void> orientToTopLeft(int priority);

    void updateLayers();
    Task<void> ensureValid(std::string_view channelSelector, int taskPriority);

    bool hasChannel(std::string_view channelName) const { return channel(channelName) != nullptr; }

    const Channel* channel(std::string_view channelName) const & {
        const auto it = std::ranges::find(channels, channelName, [](const auto& c) { return c.name(); });
        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }

    Channel* mutableChannel(std::string_view channelName) & {
        const auto it = std::ranges::find(channels, channelName, [](const auto& c) { return c.name(); });
        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }
};

struct ChannelGroup {
    std::string name;
    std::vector<std::string> channels;

    bool operator==(const ChannelGroup& other) const { return name == other.name; }
};

struct ImageTexture {
    nanogui::ref<nanogui::Texture> nanoguiTexture;
    std::vector<std::string> channels;
    bool mipmapDirty;
};

class Image {
public:
    Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, std::string_view channelSelector, bool groupChannels);
    virtual ~Image();

    const fs::path& path() const & { return mPath; }

    fs::file_time_type fileLastModified() const { return mFileLastModified; }

    void setFileLastModified(fs::file_time_type value) { mFileLastModified = value; }

    std::string_view channelSelector() const & { return mChannelSelector; }

    std::string_view name() const & { return mName; }

    std::string shortName() const;

    bool hasChannel(std::string_view channelName) const { return mData.hasChannel(channelName); }

    const Channel* channel(std::string_view channelName) const & { return mData.channel(channelName); }
    std::vector<const Channel*> channels(std::span<const std::string> channelNames) const {
        std::vector<const Channel*> result;
        for (const auto& channelName : channelNames) {
            result.push_back(channel(channelName));
        }

        return result;
    }

    bool isInterleaved(std::span<const std::string> channelNames, size_t desiredBytesPerSample, size_t desiredStride) const;

    nanogui::Texture* texture(std::span<const std::string> channelNames, EInterpolationMode minFilter, EInterpolationMode magFilter) &;

    std::span<const std::string> channelsInGroup(std::string_view groupName) const &;
    void decomposeChannelGroup(std::string_view groupName);

    nanogui::Vector2i size() const { return mData.size(); }

    nanogui::Vector2i displaySize() const { return mData.displaySize(); }

    bool contains(const nanogui::Vector2i& pos) const {
        return pos.x() >= 0 && pos.y() >= 0 && pos.x() < mData.size().x() && pos.y() < mData.size().y();
    }

    const Box2i& dataWindow() const & { return mData.dataWindow; }
    const Box2i& displayWindow() const & { return mData.displayWindow; }
    Box2i toImageCoords(const Box2i& displayWindow) const {
        return displayWindow.translate(mData.displayWindow.min - mData.dataWindow.min);
    }

    float whiteLevel() const { return mData.hdrMetadata.bestGuessWhiteLevel; }

    nanogui::Vector2f centerDisplayOffset(const Box2i& displayWindow) const {
        return Box2f{dataWindow()}.middle() - Box2f{displayWindow}.middle();
    }

    size_t numPixels() const { return mData.numPixels(); }

    std::span<const ChannelGroup> channelGroups() const & { return mChannelGroups; }

    int id() const { return mId; }

    void bumpId() {
        int oldId = mId;
        mId = sId++;

        if (mStaleIdCallback) {
            mStaleIdCallback(oldId);
        }
    }

    static int drawId() { return sId++; }

    void updateChannel(std::string_view channelName, int x, int y, int width, int height, std::span<const float> data);

    void updateVectorGraphics(bool append, std::span<const VgCommand> commands);

    std::span<const VgCommand> vgCommands() const & { return mVgCommands; }

    void setStaleIdCallback(const std::function<void(int)>& callback) { mStaleIdCallback = callback; }

    Task<std::vector<Channel>>
        getHdrImageData(std::shared_ptr<Image> reference, std::string_view requestedChannelGroup, EMetric metric, int priority) const;

    Task<HeapArray<float>> getRgbaHdrImageData(
        std::shared_ptr<Image> reference,
        const Box2i& imageRegion,
        std::string_view requestedChannelGroup,
        EMetric metric,
        const nanogui::Color& bg,
        bool divideAlpha,
        int priority
    ) const;

    Task<HeapArray<uint8_t>>
        getRgbaLdrImageData(const HeapArray<float>& hdrData, ETonemap tonemap, float gamma, float exposure, float offset, int priority) const;

    Task<HeapArray<uint8_t>> getRgbaLdrImageData(
        std::shared_ptr<Image> reference,
        const Box2i& imageRegion,
        std::string_view requestedChannelGroup,
        EMetric metric,
        const nanogui::Color& bg,
        bool divideAlpha,
        ETonemap tonemap,
        float gamma,
        float exposure,
        float offset,
        int priority
    ) const;

    Task<void> save(
        const fs::path& path,
        std::shared_ptr<Image> reference,
        const Box2i& imageRegion,
        std::string_view requestedChannelGroup,
        EMetric metric,
        const nanogui::Color& bg,
        ETonemap tonemap,
        float gamma,
        float exposure,
        float offset,
        int priority
    ) const;

    std::string toString() const;

    std::span<const AttributeNode> attributes() const & { return mData.attributes; }

private:
    static std::atomic<int> sId;

    Channel* mutableChannel(std::string_view channelName) & { return mData.mutableChannel(channelName); }

    std::vector<ChannelGroup> getGroupedChannels(std::string_view layerName) const;

    fs::path mPath;
    fs::file_time_type mFileLastModified;

    std::string mChannelSelector;

    std::string mName;

    std::map<std::string, ImageTexture> mTextures;

    ImageData mData;

    std::vector<ChannelGroup> mChannelGroups;

    std::vector<VgCommand> mVgCommands;

    std::function<void(int)> mStaleIdCallback;

    int mId;
};

// Modifies `data` and returns the new size of the data after reorientation.
Task<nanogui::Vector2i> orientToTopLeft(PixelBuffer& data, nanogui::Vector2i size, EOrientation orientation, int priority);

Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(
    int imageId, fs::path path, std::istream& iStream, std::string_view channelSelector, const ImageLoaderSettings& settings, bool groupChannels
);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(
    fs::path path, std::istream& iStream, std::string_view channelSelector, const ImageLoaderSettings& settings, bool groupChannels
);
Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(int imageId, fs::path path, std::string_view channelSelector, const ImageLoaderSettings& settings, bool groupChannels);
Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(fs::path path, std::string_view channelSelector, const ImageLoaderSettings& settings, bool groupChannels);

} // namespace tev

namespace std {

template <> struct hash<tev::ChannelGroup> {
    size_t operator()(const tev::ChannelGroup& g) const { return hash<string>()(g.name); }
};

} // namespace std
