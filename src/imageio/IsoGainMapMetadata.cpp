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

#include <tev/Common.h>
#include <tev/imageio/IsoGainMapMetadata.h>

using namespace std;

namespace tev {

enum class EFlags : uint8_t {
    BackwardDirection = 1u << 2,
    UseCommonDenominator = 1u << 3,
    UseBaseColorSpace = 1u << 6,
    IsMultiChannel = 1u << 7,
};

IsoGainMapMetadata::IsoGainMapMetadata(span<const uint8_t> data) {
    mReverseEndianess = endian::native == endian::little;

    // HACK HACK HACK: iPhone heic photos seem to have 1 extra padding byte at the beginning of the IsoGainMapMetadata box. Ignore it if
    // present. This is hard to reliably detect: there's no magic byte or similar we could advance towards. As a heuristic, we check if the
    // data size is 62 (1 more than 61 for single-channel data) or 142 (1 more than 141 for multi-channel data).
    if (data.size() == 62 || data.size() == 142) {
        data = data.subspan(1);
    }

    size_t pos = 0;

    static constexpr uint16_t LATEST_SUPPORTED_VERSION = 0;

    const uint16_t minVersion = read<uint16_t>(data, &pos);
    if (minVersion > LATEST_SUPPORTED_VERSION) {
        throw invalid_argument(fmt::format("Unsupported IsoGainMapMetadata version {}.", minVersion));
    }

    const uint16_t writerVersion = read<uint16_t>(data, &pos);
    tlog::debug() << fmt::format("IsoGainMapMetadata writer version: {}", writerVersion);

    const uint8_t flags = read<uint8_t>(data, &pos);

    const uint8_t channelCount = ((flags & (uint8_t)EFlags::IsMultiChannel) != 0) * 2 + 1;
    if (!(channelCount == 1 || channelCount == 3)) {
        throw invalid_argument(fmt::format("Unsupported IsoGainMapMetadata channel count {}, expected 1 or 3.", channelCount));
    }

    tlog::debug() << fmt::format("IsoGainMapMetadata channel count: {}", channelCount);

    mUseBaseColorSpace = flags & (uint8_t)EFlags::UseBaseColorSpace;

    // The following two flags are not actually defined by the spec, but they seem to be used like this by UltraHDR, so we follow suit.
    mBackwardDirection = flags & (uint8_t)EFlags::BackwardDirection;
    const bool useCommonDenominator = flags & (uint8_t)EFlags::UseCommonDenominator;
    tlog::debug() << fmt::format(
        "IsoGainMapMetadata flags: UseBaseColorSpace={}, BackwardDirection={}, UseCommonDenominator={}",
        mUseBaseColorSpace,
        mBackwardDirection,
        useCommonDenominator
    );

    if (useCommonDenominator) {
        const auto commonDenominator = (double)read<uint32_t>(data, &pos);

        mBaseHdrHeadroom = (double)read<uint32_t>(data, &pos) / commonDenominator;
        mAlternateHdrHeadroom = (double)read<uint32_t>(data, &pos) / commonDenominator;

        for (int c = 0; c < channelCount; ++c) {
            mGainMapMin[c] = (double)read<int32_t>(data, &pos) / commonDenominator;
            mGainMapMax[c] = (double)read<int32_t>(data, &pos) / commonDenominator;
            mGainMapGamma[c] = (double)read<uint32_t>(data, &pos) / commonDenominator;
            mBaseOffset[c] = (double)read<int32_t>(data, &pos) / commonDenominator;
            mAlternateOffset[c] = (double)read<int32_t>(data, &pos) / commonDenominator;
        }
    } else {
        mBaseHdrHeadroom = (double)read<uint32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
        mAlternateHdrHeadroom = (double)read<uint32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);

        for (int c = 0; c < channelCount; ++c) {
            mGainMapMin[c] = (double)read<int32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
            mGainMapMax[c] = (double)read<int32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
            mGainMapGamma[c] = (double)read<uint32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
            mBaseOffset[c] = (double)read<int32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
            mAlternateOffset[c] = (double)read<int32_t>(data, &pos) / (double)read<uint32_t>(data, &pos);
        }
    }

    for (int c = channelCount; c < 3; ++c) {
        mGainMapMin[c] = mGainMapMin[0];
        mGainMapMax[c] = mGainMapMax[0];
        mGainMapGamma[c] = mGainMapGamma[0];
        mBaseOffset[c] = mBaseOffset[0];
        mAlternateOffset[c] = mAlternateOffset[0];
    }
}

AttributeNode IsoGainMapMetadata::toAttributes() const {
    AttributeNode result;
    result.name = "ISO 21496-1 Gainmap";

    AttributeNode& global = result.children.emplace_back(AttributeNode{.name = "Global", .value = "", .type = "", .children = {}});

    global.children.push_back({.name = "Use Base Color Space", .value = mUseBaseColorSpace ? "true" : "false", .type = "bool", .children = {}});
    global.children.push_back({.name = "Backward Direction", .value = mBackwardDirection ? "true" : "false", .type = "bool", .children = {}});
    global.children.push_back({.name = "Base HDR Headroom", .value = fmt::format("{}", mBaseHdrHeadroom), .type = "float", .children = {}});
    global.children.push_back({.name = "Alternate HDR Headroom", .value = fmt::format("{}", mAlternateHdrHeadroom), .type = "float", .children = {}});

    for (int c = 0; c < 3; ++c) {
        AttributeNode& channelNode = result.children.emplace_back(AttributeNode{.name = fmt::format("Channel {}", c), .value = "", .type = "", .children = {}});

        channelNode.children.push_back({.name = "Gain Map Min", .value = fmt::format("{}", mGainMapMin[c]), .type = "float", .children = {}});
        channelNode.children.push_back({.name = "Gain Map Max", .value = fmt::format("{}", mGainMapMax[c]), .type = "float", .children = {}});
        channelNode.children.push_back({.name = "Gain Map Gamma", .value = fmt::format("{}", mGainMapGamma[c]), .type = "float", .children = {}});
        channelNode.children.push_back({.name = "Base Offset", .value = fmt::format("{}", mBaseOffset[c]), .type = "float", .children = {}});
        channelNode.children.push_back({.name = "Alternate Offset", .value = fmt::format("{}", mAlternateOffset[c]), .type = "float", .children = {}});
    }

    return result;
}

} // namespace tev
