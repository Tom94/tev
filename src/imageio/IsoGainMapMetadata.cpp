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

#define TXMP_STRING_TYPE std::string
#include <XMP.hpp>
#include <XMP.incl_cpp>

using namespace std;

namespace tev {

enum class EFlags : uint8_t {
    // The BackwardDirection and UseCommonDenominator flags are not defined in ISO 21496-1, but are used in practice by libultrahdr. We
    // follow suit here for compatibility.
    BackwardDirection = 1u << 2,
    UseCommonDenominator = 1u << 3,
    UseBaseColorSpace = 1u << 6,
    IsMultiChannel = 1u << 7,
};

IsoGainMapVersion::IsoGainMapVersion(span<const uint8_t> data, size_t* pos) {
    static constexpr uint16_t LATEST_SUPPORTED_VERSION = 0;

    const auto version = IsoGainMapMetadata::read<uint16_t>(data, pos);
    if (version > LATEST_SUPPORTED_VERSION) {
        throw invalid_argument(fmt::format("Unsupported IsoGainMapMetadata version {}.", version));
    }

    const auto writerVersion = IsoGainMapMetadata::read<uint16_t>(data, pos);

    tlog::debug() << fmt::format("ISO 21496-1: version={} writerVersion={}", version, writerVersion);

    mVersionString = fmt::format("ISO 21496-1 v{} / v{}", version, writerVersion);
}

IsoGainMapMetadata::IsoGainMapMetadata(span<const uint8_t> data) {
    // HACK HACK HACK: iPhone heic photos seem to have 1 extra padding byte at the beginning of the IsoGainMapMetadata box. Ignore it if
    // present. This is hard to reliably detect: there's no magic byte or similar we could advance towards. As a heuristic, we check if the
    // data size is 62 (1 more than 61 for single-channel data) or 142 (1 more than 141 for multi-channel data).
    if (data.size() == 62 || data.size() == 142) {
        data = data.subspan(1);
    }

    size_t pos = 0;

    mVersion = IsoGainMapVersion(data, &pos);

    const uint8_t flags = read<uint8_t>(data, &pos);

    const uint8_t channelCount = ((flags & (uint8_t)EFlags::IsMultiChannel) != 0) * 2 + 1;
    if (!(channelCount == 1 || channelCount == 3)) {
        throw invalid_argument(fmt::format("Unsupported IsoGainMapMetadata channel count {}, expected 1 or 3.", channelCount));
    }

    tlog::debug() << fmt::format("IsoGainMapMetadata channel count: {}", channelCount);

    mUseBaseColorSpace = flags & (uint8_t)EFlags::UseBaseColorSpace;

    const bool backwardDirection = flags & (uint8_t)EFlags::BackwardDirection;
    const bool useCommonDenominator = flags & (uint8_t)EFlags::UseCommonDenominator;
    tlog::debug() << fmt::format(
        "IsoGainMapMetadata flags: useBaseColorSpace={} backwardDirection={} useCommonDenominator={}",
        mUseBaseColorSpace,
        backwardDirection,
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
        mBaseHdrHeadroom = (double)read<uint32_t>(data, &pos);
        mBaseHdrHeadroom /= (double)read<uint32_t>(data, &pos);

        mAlternateHdrHeadroom = (double)read<uint32_t>(data, &pos);
        mAlternateHdrHeadroom /= (double)read<uint32_t>(data, &pos);

        for (int c = 0; c < channelCount; ++c) {
            mGainMapMin[c] = (double)read<int32_t>(data, &pos);
            mGainMapMin[c] /= (double)read<uint32_t>(data, &pos);

            mGainMapMax[c] = (double)read<int32_t>(data, &pos);
            mGainMapMax[c] /= (double)read<uint32_t>(data, &pos);

            mGainMapGamma[c] = (double)read<uint32_t>(data, &pos);
            mGainMapGamma[c] /= (double)read<uint32_t>(data, &pos);

            mBaseOffset[c] = (double)read<int32_t>(data, &pos);
            mBaseOffset[c] /= (double)read<uint32_t>(data, &pos);

            mAlternateOffset[c] = (double)read<int32_t>(data, &pos);
            mAlternateOffset[c] /= (double)read<uint32_t>(data, &pos);
        }
    }

    for (int c = channelCount; c < 3; ++c) {
        mGainMapMin[c] = mGainMapMin[0];
        mGainMapMax[c] = mGainMapMax[0];
        mGainMapGamma[c] = mGainMapGamma[0];
        mBaseOffset[c] = mBaseOffset[0];
        mAlternateOffset[c] = mAlternateOffset[0];
    }

    if (backwardDirection) {
        reverseDirection();
    }
}

IsoGainMapMetadata::IsoGainMapMetadata(const char* ns, void* xmpMeta) {
    SXMPMeta* meta = reinterpret_cast<SXMPMeta*>(xmpMeta);

    try {
        string version;
        if (!meta->GetProperty(ns, "Version", &version, nullptr)) {
            throw invalid_argument{"XMP gainmap property Version is required."};
        }

        mVersion = IsoGainMapVersion{fmt::format("XMP v{}", version)};

        const auto getMaybeRgbFloat = [&](const char* name, nanogui::Vector3f& out) {
            if (XMP_OptionBits options; meta->GetProperty(ns, name, nullptr, &options)) {
                if (options & kXMP_PropValueIsArray) {
                    XMP_Index count = meta->CountArrayItems(ns, name);
                    if (count != 1 && count != 3) {
                        throw invalid_argument(
                            fmt::format("XMP gainmap property '{}' has invalid number of array items {}, expected 1 or 3.", name, count)
                        );
                    }

                    for (XMP_Index i = 0; i < count; ++i) {
                        string path;
                        SXMPUtils::ComposeArrayItemPath(ns, name, i + 1, &path);

                        if (double val; meta->GetProperty_Float(ns, path.c_str(), &val, nullptr)) {
                            out[i] = (float)val;
                        } else {
                            throw invalid_argument(fmt::format("XMP gainmap property '{}' array item {} is not a float.", name, i));
                        }
                    }

                    // Fill up remaining channels with first channel if only one was given
                    for (size_t i = count; i < 3; ++i) {
                        out[i] = out[0];
                    }
                } else {
                    string val;
                    if (!meta->GetProperty(ns, name, &val, nullptr)) {
                        throw invalid_argument(fmt::format("XMP gainmap property '{}' should exist.", name));
                    }

                    const auto parts = split(val, ",");
                    if (parts.size() != 1 && parts.size() != 3) {
                        throw invalid_argument(
                            fmt::format(
                                "XMP gainmap property '{}' has invalid number of comma-separated values {}, expected 1 or 3.", name, parts.size()
                            )
                        );
                    }

                    for (size_t i = 0; i < parts.size(); ++i) {
                        try {
                            out[i] = stof(string{parts[i]});
                        } catch (const invalid_argument&) {
                            throw invalid_argument(fmt::format("XMP gainmap property '{}' value '{}' is not a float.", name, parts[i]));
                        }
                    }

                    // Fill up remaining channels with first channel if only one was given
                    for (size_t i = parts.size(); i < 3; ++i) {
                        out[i] = out[0];
                    }
                }

                return true;
            }

            return false;
        };

        const auto getFloat = [&](const char* name, float& out) {
            if (double val; meta->GetProperty_Float(ns, name, &val, nullptr)) {
                out = (float)val;
                return true;
            }

            return false;
        };

        if (!getMaybeRgbFloat("GainMapMin", mGainMapMin)) {
            mGainMapMin = nanogui::Vector3f{0.0f};
        }

        if (!getMaybeRgbFloat("GainMapMax", mGainMapMax)) {
            throw invalid_argument{"XMP gainmap property GainMapMax is required."};
        }

        mGainMapMax = max(mGainMapMax, mGainMapMin);

        if (!getMaybeRgbFloat("Gamma", mGainMapGamma)) {
            mGainMapGamma = nanogui::Vector3f{1.0f};
        }

        mGainMapGamma = max(mGainMapGamma, nanogui::Vector3f{0.001f});

        if (!getMaybeRgbFloat("OffsetSDR", mBaseOffset)) {
            mBaseOffset = nanogui::Vector3f{1.0f / 64.0f};
        }

        mBaseOffset = max(mBaseOffset, nanogui::Vector3f{0.0f});

        if (!getMaybeRgbFloat("OffsetHDR", mAlternateOffset)) {
            mAlternateOffset = nanogui::Vector3f{1.0f / 64.0f};
        }

        mAlternateOffset = max(mAlternateOffset, nanogui::Vector3f{0.0f});

        if (!getFloat("HDRCapacityMin", mBaseHdrHeadroom)) {
            mBaseHdrHeadroom = 0.0f;
        }

        if (!getFloat("HDRCapacityMax", mAlternateHdrHeadroom)) {
            throw invalid_argument{"XMP gainmap property HDRCapacityMax is required."};
        }

        mAlternateHdrHeadroom = max(mAlternateHdrHeadroom, mBaseHdrHeadroom);

        // Old versions of XMP gainmap metadata used different properties to indicate the gainmap direction.
        if (string baseRendition; meta->GetProperty(ns, "BaseRendition", &baseRendition, nullptr)) {
            if (baseRendition == "HDR" || baseRendition == "HighDynamicRange") {
                reverseDirection();
            }
        } else if (bool backwardDirection;
                   meta->GetProperty_Bool(ns, "BaseRenditionIsHDR", &backwardDirection, nullptr) && backwardDirection) {
            reverseDirection();
        }
    } catch (const XMP_Error& e) {
        throw invalid_argument(fmt::format("Failed to read ISO 21496-1 gainmap XMP metadata: {}", e.GetErrMsg()));
    }
}

AttributeNode IsoGainMapMetadata::toAttributes() const {
    AttributeNode result;
    result.name = "Gain map";

    AttributeNode& global = result.children.emplace_back(AttributeNode{.name = "Global", .value = "", .type = "", .children = {}});

    global.children.push_back({.name = "Version", .value = mVersion.toString(), .type = "string", .children = {}});

    global.children.push_back({.name = "Use Base Color Space", .value = mUseBaseColorSpace ? "true" : "false", .type = "bool", .children = {}});
    global.children.push_back({.name = "Base HDR Headroom", .value = fmt::format("{}", mBaseHdrHeadroom), .type = "float", .children = {}});
    global.children.push_back(
        {.name = "Alternate HDR Headroom", .value = fmt::format("{}", mAlternateHdrHeadroom), .type = "float", .children = {}}
    );

    for (int c = 0; c < 3; ++c) {
        AttributeNode& channelNode =
            result.children.emplace_back(AttributeNode{.name = fmt::format("Channel {}", c), .value = "", .type = "", .children = {}});

        channelNode.children.push_back({.name = "Gain Map Min", .value = fmt::format("{}", mGainMapMin[c]), .type = "float", .children = {}});
        channelNode.children.push_back({.name = "Gain Map Max", .value = fmt::format("{}", mGainMapMax[c]), .type = "float", .children = {}});
        channelNode.children.push_back(
            {.name = "Gain Map Gamma", .value = fmt::format("{}", mGainMapGamma[c]), .type = "float", .children = {}}
        );
        channelNode.children.push_back({.name = "Base Offset", .value = fmt::format("{}", mBaseOffset[c]), .type = "float", .children = {}});
        channelNode.children.push_back(
            {.name = "Alternate Offset", .value = fmt::format("{}", mAlternateOffset[c]), .type = "float", .children = {}}
        );
    }

    return result;
}

void IsoGainMapMetadata::reverseDirection() {
    // Swap base and alternate parameters
    swap(mBaseHdrHeadroom, mAlternateHdrHeadroom);
    swap(mBaseOffset, mAlternateOffset);
}

} // namespace tev
