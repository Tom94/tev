/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Colors.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/KtxImageLoader.h>

#include <ktx.h>

#include <half.h>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {

// VkFormat values we can read directly. Spelled out numerically so that libktx remains our only dependency -- ktx.h exposes vkFormat as a
// plain uint32 and does not pull in the Vulkan headers.
enum : uint32_t {
    VK_FORMAT_UNDEFINED = 0,
    VK_FORMAT_R8_UNORM = 9,
    VK_FORMAT_R8_SRGB = 15,
    VK_FORMAT_R8G8_UNORM = 16,
    VK_FORMAT_R8G8_SRGB = 22,
    VK_FORMAT_R8G8B8_UNORM = 23,
    VK_FORMAT_R8G8B8_SRGB = 29,
    VK_FORMAT_B8G8R8_UNORM = 30,
    VK_FORMAT_B8G8R8_SRGB = 36,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_R8G8B8A8_SRGB = 43,
    VK_FORMAT_B8G8R8A8_UNORM = 44,
    VK_FORMAT_B8G8R8A8_SRGB = 50,
    VK_FORMAT_R16_UNORM = 70,
    VK_FORMAT_R16_SFLOAT = 76,
    VK_FORMAT_R16G16_UNORM = 77,
    VK_FORMAT_R16G16_SFLOAT = 83,
    VK_FORMAT_R16G16B16_UNORM = 84,
    VK_FORMAT_R16G16B16_SFLOAT = 90,
    VK_FORMAT_R16G16B16A16_UNORM = 91,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32_SFLOAT = 100,
    VK_FORMAT_R32G32_SFLOAT = 103,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109,
    // ASTC LDR occupies a contiguous core range; ASTC HDR lives in the VK_EXT_texture_compression_astc_hdr extension range.
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157,
    VK_FORMAT_ASTC_12x12_SRGB_BLOCK = 184,
    VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK = 1000066000,
    VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK = 1000066013,
};

bool isAstcVkFormat(const uint32_t format) {
    return (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
        (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

// Subset of GL internal formats needed for KTX1. KTX1 predates VkFormat and describes its payload with the GL enum triple instead.
enum : uint32_t {
    GL_LUMINANCE = 0x1909,
    GL_LUMINANCE_ALPHA = 0x190A,
    GL_RGB = 0x1907,
    GL_RGBA = 0x1908,
    GL_R8 = 0x8229,
    GL_R16 = 0x822A,
    GL_RG8 = 0x822B,
    GL_RG16 = 0x822C,
    GL_R16F = 0x822D,
    GL_R32F = 0x822E,
    GL_RG16F = 0x822F,
    GL_RG32F = 0x8230,
    GL_RGB8 = 0x8051,
    GL_RGB16 = 0x8054,
    GL_RGBA8 = 0x8058,
    GL_RGBA16 = 0x805B,
    GL_RGBA32F = 0x8814,
    GL_RGB32F = 0x8815,
    GL_RGBA16F = 0x881A,
    GL_RGB16F = 0x881B,
    GL_SRGB8 = 0x8C41,
    GL_SRGB8_ALPHA8 = 0x8C43,
};

enum class ESampleType {
    UNorm8,
    UNorm16,
    Float16,
    Float32,
};

struct SampleLayout {
    size_t numChannels = 0;
    ESampleType type = ESampleType::UNorm8;
    bool srgb = false;
    bool bgr = false;

    size_t bytesPerSample() const {
        switch (type) {
            case ESampleType::UNorm8: return 1;
            case ESampleType::UNorm16:
            case ESampleType::Float16: return 2;
            case ESampleType::Float32: return 4;
        }

        return 0;
    }
};

optional<SampleLayout> layoutFromVkFormat(const uint32_t format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return SampleLayout{1, ESampleType::UNorm8, false, false};
        case VK_FORMAT_R8_SRGB: return SampleLayout{1, ESampleType::UNorm8, true, false};
        case VK_FORMAT_R8G8_UNORM: return SampleLayout{2, ESampleType::UNorm8, false, false};
        case VK_FORMAT_R8G8_SRGB: return SampleLayout{2, ESampleType::UNorm8, true, false};
        case VK_FORMAT_R8G8B8_UNORM: return SampleLayout{3, ESampleType::UNorm8, false, false};
        case VK_FORMAT_R8G8B8_SRGB: return SampleLayout{3, ESampleType::UNorm8, true, false};
        case VK_FORMAT_B8G8R8_UNORM: return SampleLayout{3, ESampleType::UNorm8, false, true};
        case VK_FORMAT_B8G8R8_SRGB: return SampleLayout{3, ESampleType::UNorm8, true, true};
        case VK_FORMAT_R8G8B8A8_UNORM: return SampleLayout{4, ESampleType::UNorm8, false, false};
        case VK_FORMAT_R8G8B8A8_SRGB: return SampleLayout{4, ESampleType::UNorm8, true, false};
        case VK_FORMAT_B8G8R8A8_UNORM: return SampleLayout{4, ESampleType::UNorm8, false, true};
        case VK_FORMAT_B8G8R8A8_SRGB: return SampleLayout{4, ESampleType::UNorm8, true, true};

        case VK_FORMAT_R16_UNORM: return SampleLayout{1, ESampleType::UNorm16, false, false};
        case VK_FORMAT_R16G16_UNORM: return SampleLayout{2, ESampleType::UNorm16, false, false};
        case VK_FORMAT_R16G16B16_UNORM: return SampleLayout{3, ESampleType::UNorm16, false, false};
        case VK_FORMAT_R16G16B16A16_UNORM: return SampleLayout{4, ESampleType::UNorm16, false, false};

        case VK_FORMAT_R16_SFLOAT: return SampleLayout{1, ESampleType::Float16, false, false};
        case VK_FORMAT_R16G16_SFLOAT: return SampleLayout{2, ESampleType::Float16, false, false};
        case VK_FORMAT_R16G16B16_SFLOAT: return SampleLayout{3, ESampleType::Float16, false, false};
        case VK_FORMAT_R16G16B16A16_SFLOAT: return SampleLayout{4, ESampleType::Float16, false, false};

        case VK_FORMAT_R32_SFLOAT: return SampleLayout{1, ESampleType::Float32, false, false};
        case VK_FORMAT_R32G32_SFLOAT: return SampleLayout{2, ESampleType::Float32, false, false};
        case VK_FORMAT_R32G32B32_SFLOAT: return SampleLayout{3, ESampleType::Float32, false, false};
        case VK_FORMAT_R32G32B32A32_SFLOAT: return SampleLayout{4, ESampleType::Float32, false, false};

        default: return nullopt;
    }
}

optional<SampleLayout> layoutFromGlInternalFormat(const uint32_t format) {
    switch (format) {
        case GL_LUMINANCE:
        case GL_R8: return SampleLayout{1, ESampleType::UNorm8, false, false};
        case GL_LUMINANCE_ALPHA:
        case GL_RG8: return SampleLayout{2, ESampleType::UNorm8, false, false};
        case GL_RGB:
        case GL_RGB8: return SampleLayout{3, ESampleType::UNorm8, false, false};
        case GL_RGBA:
        case GL_RGBA8: return SampleLayout{4, ESampleType::UNorm8, false, false};

        case GL_SRGB8: return SampleLayout{3, ESampleType::UNorm8, true, false};
        case GL_SRGB8_ALPHA8: return SampleLayout{4, ESampleType::UNorm8, true, false};

        case GL_R16: return SampleLayout{1, ESampleType::UNorm16, false, false};
        case GL_RG16: return SampleLayout{2, ESampleType::UNorm16, false, false};
        case GL_RGB16: return SampleLayout{3, ESampleType::UNorm16, false, false};
        case GL_RGBA16: return SampleLayout{4, ESampleType::UNorm16, false, false};

        case GL_R16F: return SampleLayout{1, ESampleType::Float16, false, false};
        case GL_RG16F: return SampleLayout{2, ESampleType::Float16, false, false};
        case GL_RGB16F: return SampleLayout{3, ESampleType::Float16, false, false};
        case GL_RGBA16F: return SampleLayout{4, ESampleType::Float16, false, false};

        case GL_R32F: return SampleLayout{1, ESampleType::Float32, false, false};
        case GL_RG32F: return SampleLayout{2, ESampleType::Float32, false, false};
        case GL_RGB32F: return SampleLayout{3, ESampleType::Float32, false, false};
        case GL_RGBA32F: return SampleLayout{4, ESampleType::Float32, false, false};

        default: return nullopt;
    }
}

struct KtxTextureDeleter {
    void operator()(ktxTexture* texture) const {
        if (texture) {
            ktxTexture_Destroy(texture);
        }
    }
};

using KtxTexturePtr = unique_ptr<ktxTexture, KtxTextureDeleter>;

// KTX stores orientation in the KTXorientation metadata key. KTX1 spells it "S=r,T=d", KTX2 "rd". The GL-flavored default is bottom-up, so
// a missing or "T=u"/"ru" value means we have to flip.
bool needsVerticalFlip(ktxTexture* texture) {
    ktx_uint32_t valueLen = 0;
    char* value = nullptr;

    if (ktxHashList_FindValue(&texture->kvDataHead, KTX_ORIENTATION_KEY, &valueLen, (void**)&value) != KTX_SUCCESS || !value) {
        // No orientation metadata. KTX1 defaults to bottom-up, KTX2 to top-down.
        return texture->classId == ktxTexture1_c;
    }

    const string_view orientation{value, valueLen > 0 ? valueLen - 1 : 0};
    return orientation.find('u') != string_view::npos;
}

string makePartName(ktxTexture* texture, const uint32_t level, const uint32_t layer, const uint32_t face, const uint32_t slice) {
    static const array<string_view, 6> CUBE_FACE_NAMES = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

    vector<string> components;

    if (texture->isArray && texture->numLayers > 1) {
        components.emplace_back(fmt::format("layer{}", layer));
    }

    if (texture->isCubemap && texture->numFaces > 1) {
        components.emplace_back(CUBE_FACE_NAMES[std::min<size_t>(face, CUBE_FACE_NAMES.size() - 1)]);
    }

    if (texture->baseDepth > 1) {
        components.emplace_back(fmt::format("z{}", slice));
    }

    if (texture->numLevels > 1) {
        components.emplace_back(fmt::format("mip{}", level));
    }

    return join(components, ".");
}

template <typename T> float normalize(const T value);

template <> float normalize<uint8_t>(const uint8_t value) { return value / 255.0f; }
template <> float normalize<uint16_t>(const uint16_t value) { return value / 65535.0f; }
template <> float normalize<half>(const half value) { return static_cast<float>(value); }
template <> float normalize<float>(const float value) { return value; }

template <typename T>
Task<void> convertToFloat(
    const span<const uint8_t> src,
    const size_t srcRowPitch,
    const MultiChannelView<float>& dst,
    const SampleLayout& layout,
    const bool flipY,
    const int priority
) {
    const size_t numChannels = std::min(layout.numChannels, dst.nChannels());
    const bool srgb = layout.srgb;
    // Only the color channels of a BGR(A) image are swizzled; alpha stays put.
    const auto numSwizzled = layout.bgr ? std::min(numChannels, 3uz) : 0uz;
    const auto size = dst.size();

    co_await ThreadPool::global().parallelFor<int>(
        0,
        size.y(),
        posProd(size) * numChannels * approxCost(srgb ? ituth273::ETransfer::SRGB : ituth273::ETransfer::Linear),
        [&](int y) {
            const int srcY = flipY ? size.y() - 1 - y : y;
            const T* __restrict srcRow = reinterpret_cast<const T*>(&src[static_cast<size_t>(srcY) * srcRowPitch]);

            for (int x = 0; x < size.x(); ++x) {
                for (size_t c = 0; c < numChannels; ++c) {
                    const int srcC = c < numSwizzled ? numSwizzled - 1 - c : c;
                    float value = normalize<T>(srcRow[x * numChannels + srcC]);

                    // Alpha is always stored linearly, even in sRGB formats.
                    if (srgb && c < 3) {
                        value = ituth273::srgbToLinear(value);
                    }

                    dst[c, x, y] = value;
                }
            }
        },
        priority
    );
}

} // namespace

bool isKtxImage(istream& iStream) {
    static const uint8_t KTX1_IDENTIFIER[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t KTX2_IDENTIFIER[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

    array<uint8_t, std::max(sizeof(KTX1_IDENTIFIER), sizeof(KTX2_IDENTIFIER))> identifier{};
    iStream.read(reinterpret_cast<char*>(identifier.data()), identifier.size());

    const bool result = !!iStream && iStream.gcount() == identifier.size() &&
        (ranges::equal(identifier, KTX1_IDENTIFIER) || ranges::equal(identifier, KTX2_IDENTIFIER));

    iStream.clear();
    iStream.seekg(0);

    return result;
}

Task<vector<ImageData>>
    KtxImageLoader::load(istringstream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    if (!isKtxImage(iStream)) {
        throw FormatNotSupported{"File is not a KTX image."};
    }

    const string buffer = iStream.str();

    ktxTexture* rawTexture = nullptr;
    if (const KTX_error_code err = ktxTexture_CreateFromMemory(
            reinterpret_cast<const ktx_uint8_t*>(buffer.data()), buffer.size(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &rawTexture
        );
        err != KTX_SUCCESS) {
        throw ImageLoadError{fmt::format("Failed to parse KTX image: {}", ktxErrorString(err))};
    }

    const KtxTexturePtr texture{rawTexture};

    bool premultipliedAlpha = false;

    // KTX2 payloads may need decoding before we can look at them as plain samples. Supercompression
    // (Zstd/ZLIB) was already undone by the LOAD_IMAGE_DATA flag above; what remains is Basis and ASTC.
    if (texture->classId == ktxTexture2_c) {
        ktxTexture2* texture2 = reinterpret_cast<ktxTexture2*>(texture.get());

        premultipliedAlpha = ktxTexture2_GetPremultipliedAlpha(texture2);

        if (ktxTexture2_NeedsTranscoding(texture2)) {
            // BasisLZ/ETC1S and UASTC. RGBA32 here means 8 bits per component, so HDR UASTC payloads
            // would clamp. Transcode those to BC6H instead and hand them to the block decoder below
            // once that path exists.
            if (const KTX_error_code err = ktxTexture2_TranscodeBasis(texture2, KTX_TTF_RGBA32, 0); err != KTX_SUCCESS) {
                throw ImageLoadError{fmt::format("Failed to transcode KTX image: {}", ktxErrorString(err))};
            }
        } else if (isAstcVkFormat(texture2->vkFormat)) {
            // Decodes to RGBA8, SRGBA8 or RGBA32 (float) depending on whether the payload is LDR or HDR.
            if (const KTX_error_code err = ktxTexture2_DecodeAstc(texture2); err != KTX_SUCCESS) {
                throw ImageLoadError{fmt::format("Failed to decode ASTC payload: {}", ktxErrorString(err))};
            }
        }
    }

    // Resolve the sample layout from whichever format description this KTX version carries.
    optional<SampleLayout> layout;
    string formatName;

    if (texture->classId == ktxTexture2_c) {
        const uint32_t vkFormat = reinterpret_cast<ktxTexture2*>(texture.get())->vkFormat;
        layout = layoutFromVkFormat(vkFormat);
        formatName = fmt::format("VkFormat {}", vkFormat);
    } else {
        const uint32_t glInternalFormat = reinterpret_cast<ktxTexture1*>(texture.get())->glInternalformat;
        layout = layoutFromGlInternalFormat(glInternalFormat);
        formatName = fmt::format("GL internal format {:#x}", glInternalFormat);
    }

    if (!layout) {
        // Everything that survives to here is block compressed: BCn and ETC/EAC in KTX1, plus BCn in
        // KTX2. libktx does not expose a decoder for those, so they need bcdec/etcdec on top.
        throw ImageLoadError{fmt::format("Unsupported KTX pixel format ({}).", formatName)};
    }

    if (texture->isCompressed) {
        throw ImageLoadError{fmt::format("Unsupported block-compressed KTX pixel format ({}).", formatName)};
    }

    tlog::debug(
        "ktx image: size={} format={} channels={} premultipliedAlpha={}",
        Vector3i{(int)texture->baseWidth, (int)texture->baseHeight, (int)texture->baseDepth},
        formatName,
        layout->numChannels,
        premultipliedAlpha
    );

    const bool flipY = needsVerticalFlip(texture.get());
    const bool hasAlpha = layout->numChannels == 2 || layout->numChannels == 4;

    const span<const uint8_t> data = {ktxTexture_GetData(texture.get()), ktxTexture_GetDataSize(texture.get())};

    vector<ImageData> result;

    for (uint32_t level = 0; level < texture->numLevels; ++level) {
        const Vector2i levelSize{
            (int)std::max(1u, texture->baseWidth >> level),
            (int)std::max(1u, texture->baseHeight >> level),
        };

        const uint32_t numSlices = std::max(1u, texture->baseDepth >> level);
        const size_t rowPitch = ktxTexture_GetRowPitch(texture.get(), level);

        for (uint32_t layer = 0; layer < texture->numLayers; ++layer) {
            for (uint32_t face = 0; face < texture->numFaces; ++face) {
                for (uint32_t slice = 0; slice < numSlices; ++slice) {
                    // For 3D textures libktx indexes depth slices through the faceSlice parameter.
                    const uint32_t faceSlice = texture->numFaces > 1 ? face : slice;

                    size_t offset = 0;
                    if (const KTX_error_code err = ktxTexture_GetImageOffset(texture.get(), level, layer, faceSlice, &offset);
                        err != KTX_SUCCESS) {
                        throw ImageLoadError{fmt::format("Failed to locate KTX subimage: {}", ktxErrorString(err))};
                    }

                    const size_t imageSize = rowPitch * levelSize.y();
                    if (offset + imageSize > data.size()) {
                        throw ImageLoadError{"KTX subimage extends past the end of the payload."};
                    }

                    ImageData& resultData = result.emplace_back();
                    resultData.channels = co_await makeInterleavedChannels(
                        layout->numChannels,
                        hasAlpha,
                        levelSize,
                        EPixelFormat::F32,
                        layout->type == ESampleType::Float32 ? EPixelFormat::F32 : EPixelFormat::F16,
                        "",
                        priority
                    );

                    resultData.hasPremultipliedAlpha = hasAlpha && premultipliedAlpha;
                    resultData.partName = makePartName(texture.get(), level, layer, face, slice);

                    const auto dst = MultiChannelView<float>{resultData.channels};
                    const span<const uint8_t> src = data.subspan(offset, imageSize);

                    switch (layout->type) {
                        case ESampleType::UNorm8: co_await convertToFloat<uint8_t>(src, rowPitch, dst, *layout, flipY, priority); break;
                        case ESampleType::UNorm16: co_await convertToFloat<uint16_t>(src, rowPitch, dst, *layout, flipY, priority); break;
                        case ESampleType::Float16: co_await convertToFloat<half>(src, rowPitch, dst, *layout, flipY, priority); break;
                        case ESampleType::Float32: co_await convertToFloat<float>(src, rowPitch, dst, *layout, flipY, priority); break;
                    }
                }
            }
        }
    }

    if (result.empty()) {
        throw ImageLoadError{"KTX image contains no subimages."};
    }

    co_return result;
}

} // namespace tev
