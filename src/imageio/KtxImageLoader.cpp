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

// BCDEC_BC4BC5_PRECISE enables signed BC4/BC5 and the float decoders, but also changes the bcdec_bc4/bc5 signatures. If another TU ever
// includes bcdec, move this define to the build system so all TUs agree, and keep BCDEC_IMPLEMENTATION in exactly one of them.
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include <bcdec/bcdec.h>

#define ETCDEC_IMPLEMENTATION
#include <etcdec/etcdec.h>

#include <KTX-Software/lib/src/gl_format.h>
#include <KTX-Software/lib/src/vk_format.h>

#include <ktx.h>

#include <half.h>

#include <cmath>
#include <cstring>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {

bool isAstcVkFormat(const uint32_t format) {
    return (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
        (format >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && format <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

enum class ESampleType {
    UNorm8,
    UNorm16,
    Float16,
    Float32,
};

struct SampleLayout {
    // Interleaved channels per pixel in the source. May exceed the number of output channels: BC1 RGB and ETC2 RGB decode to RGBA8 whose
    // alpha must be dropped.
    size_t numChannels = 0;
    ESampleType type = ESampleType::UNorm8;
    // Whether the last channel is alpha. Must be explicit because 2-channel formats can be either luminance-alpha or RG.
    bool alpha = false;
    bool srgb = false;
    bool bgr = false;

    size_t numOutputChannels() const { return alpha ? numChannels : std::min(numChannels, 3uz); }

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

// Field order: {numChannels, type, alpha, srgb, bgr}
optional<SampleLayout> layoutFromVkFormat(const uint32_t format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return SampleLayout{1, ESampleType::UNorm8, false, false, false};
        case VK_FORMAT_R8_SRGB: return SampleLayout{1, ESampleType::UNorm8, false, true, false};
        case VK_FORMAT_R8G8_UNORM: return SampleLayout{2, ESampleType::UNorm8, false, false, false};
        case VK_FORMAT_R8G8_SRGB: return SampleLayout{2, ESampleType::UNorm8, false, true, false};
        case VK_FORMAT_R8G8B8_UNORM: return SampleLayout{3, ESampleType::UNorm8, false, false, false};
        case VK_FORMAT_R8G8B8_SRGB: return SampleLayout{3, ESampleType::UNorm8, false, true, false};
        case VK_FORMAT_B8G8R8_UNORM: return SampleLayout{3, ESampleType::UNorm8, false, false, true};
        case VK_FORMAT_B8G8R8_SRGB: return SampleLayout{3, ESampleType::UNorm8, false, true, true};
        case VK_FORMAT_R8G8B8A8_UNORM: return SampleLayout{4, ESampleType::UNorm8, true, false, false};
        case VK_FORMAT_R8G8B8A8_SRGB: return SampleLayout{4, ESampleType::UNorm8, true, true, false};
        case VK_FORMAT_B8G8R8A8_UNORM: return SampleLayout{4, ESampleType::UNorm8, true, false, true};
        case VK_FORMAT_B8G8R8A8_SRGB: return SampleLayout{4, ESampleType::UNorm8, true, true, true};

        case VK_FORMAT_R16_UNORM: return SampleLayout{1, ESampleType::UNorm16, false, false, false};
        case VK_FORMAT_R16G16_UNORM: return SampleLayout{2, ESampleType::UNorm16, false, false, false};
        case VK_FORMAT_R16G16B16_UNORM: return SampleLayout{3, ESampleType::UNorm16, false, false, false};
        case VK_FORMAT_R16G16B16A16_UNORM: return SampleLayout{4, ESampleType::UNorm16, true, false, false};

        case VK_FORMAT_R16_SFLOAT: return SampleLayout{1, ESampleType::Float16, false, false, false};
        case VK_FORMAT_R16G16_SFLOAT: return SampleLayout{2, ESampleType::Float16, false, false, false};
        case VK_FORMAT_R16G16B16_SFLOAT: return SampleLayout{3, ESampleType::Float16, false, false, false};
        case VK_FORMAT_R16G16B16A16_SFLOAT: return SampleLayout{4, ESampleType::Float16, true, false, false};

        case VK_FORMAT_R32_SFLOAT: return SampleLayout{1, ESampleType::Float32, false, false, false};
        case VK_FORMAT_R32G32_SFLOAT: return SampleLayout{2, ESampleType::Float32, false, false, false};
        case VK_FORMAT_R32G32B32_SFLOAT: return SampleLayout{3, ESampleType::Float32, false, false, false};
        case VK_FORMAT_R32G32B32A32_SFLOAT: return SampleLayout{4, ESampleType::Float32, true, false, false};

        default: return nullopt;
    }
}

// Field order: {numChannels, type, alpha, srgb, bgr}
optional<SampleLayout> layoutFromGlInternalFormat(const uint32_t format) {
    switch (format) {
        case GL_LUMINANCE:
        case GL_R8: return SampleLayout{1, ESampleType::UNorm8, false, false, false};
        case GL_LUMINANCE_ALPHA: return SampleLayout{2, ESampleType::UNorm8, true, false, false};
        case GL_RG8: return SampleLayout{2, ESampleType::UNorm8, false, false, false};
        case GL_RGB:
        case GL_RGB8: return SampleLayout{3, ESampleType::UNorm8, false, false, false};
        case GL_RGBA:
        case GL_RGBA8: return SampleLayout{4, ESampleType::UNorm8, true, false, false};

        case GL_SRGB8: return SampleLayout{3, ESampleType::UNorm8, false, true, false};
        case GL_SRGB8_ALPHA8: return SampleLayout{4, ESampleType::UNorm8, true, true, false};

        case GL_R16: return SampleLayout{1, ESampleType::UNorm16, false, false, false};
        case GL_RG16: return SampleLayout{2, ESampleType::UNorm16, false, false, false};
        case GL_RGB16: return SampleLayout{3, ESampleType::UNorm16, false, false, false};
        case GL_RGBA16: return SampleLayout{4, ESampleType::UNorm16, true, false, false};

        case GL_R16F: return SampleLayout{1, ESampleType::Float16, false, false, false};
        case GL_RG16F: return SampleLayout{2, ESampleType::Float16, false, false, false};
        case GL_RGB16F: return SampleLayout{3, ESampleType::Float16, false, false, false};
        case GL_RGBA16F: return SampleLayout{4, ESampleType::Float16, true, false, false};

        case GL_R32F: return SampleLayout{1, ESampleType::Float32, false, false, false};
        case GL_RG32F: return SampleLayout{2, ESampleType::Float32, false, false, false};
        case GL_RGB32F: return SampleLayout{3, ESampleType::Float32, false, false, false};
        case GL_RGBA32F: return SampleLayout{4, ESampleType::Float32, true, false, false};

        default: return nullopt;
    }
}

// Formats that can't be read sample-by-sample and are first decoded into an intermediate buffer: block-compressed BCn/ETC/EAC, and the
// packed float formats, which are treated as 1x1 "blocks" so they share the same path.
enum class EEncoding {
    BC1,
    BC1A,
    BC2,
    BC3,
    BC4,
    BC5,
    BC6H,
    BC7,
    ETC2_RGB,
    ETC2_RGB_A1,
    ETC2_RGBA,
    EAC_R11,
    EAC_RG11,
    B10G11R11,
    E5B9G9R9,
};

struct EncodedLayout {
    EEncoding encoding;
    bool srgb = false;
    bool isSigned = false;

    int blockDim() const { return encoding == EEncoding::B10G11R11 || encoding == EEncoding::E5B9G9R9 ? 1 : 4; }

    size_t blockBytes() const {
        switch (encoding) {
            case EEncoding::B10G11R11:
            case EEncoding::E5B9G9R9: return 4;
            case EEncoding::BC1:
            case EEncoding::BC1A:
            case EEncoding::BC4:
            case EEncoding::ETC2_RGB:
            case EEncoding::ETC2_RGB_A1:
            case EEncoding::EAC_R11: return 8;
            default: return 16;
        }
    }

    size_t encodedSize(const Vector2i& size) const {
        const int dim = blockDim();
        return (size_t)((size.x() + dim - 1) / dim) * (size_t)((size.y() + dim - 1) / dim) * blockBytes();
    }

    // Unit, in bytes, in which the decoder expects its destination pitch. bcdec's float and half decoders step a typed
    // pointer, everything else (including etcdec's float decoders) steps bytes.
    size_t pitchUnitBytes() const {
        switch (encoding) {
            case EEncoding::BC4:
            case EEncoding::BC5: return sizeof(float);
            case EEncoding::BC6H: return sizeof(uint16_t);
            default: return 1;
        }
    }

    // Field order: {numChannels, type, alpha, srgb, bgr}
    SampleLayout decodedLayout() const {
        switch (encoding) {
            // Opaque BC1 and ETC2 RGB still decode to RGBA8, and BC1's punch-through blocks write alpha 0. Drop the alpha channel.
            case EEncoding::BC1:
            case EEncoding::ETC2_RGB: return {4, ESampleType::UNorm8, false, srgb, false};
            case EEncoding::BC1A:
            case EEncoding::BC2:
            case EEncoding::BC3:
            case EEncoding::BC7:
            case EEncoding::ETC2_RGB_A1:
            case EEncoding::ETC2_RGBA: return {4, ESampleType::UNorm8, true, srgb, false};
            // BC4/BC5 go through bcdec's float decoders so that SNORM variants land in [-1,1] without a signed sample type.
            case EEncoding::BC4: return {1, ESampleType::Float32, false, false, false};
            case EEncoding::BC5: return {2, ESampleType::Float32, false, false, false};
            // BC6H's native precision is half.
            case EEncoding::BC6H: return {3, ESampleType::Float16, false, false, false};
            // etcdec expands the 11-bit EAC values to 16 bits.
            case EEncoding::EAC_R11: return {1, ESampleType::UNorm16, false, false, false};
            case EEncoding::EAC_RG11: return {2, ESampleType::UNorm16, false, false, false};
            case EEncoding::B10G11R11:
            case EEncoding::E5B9G9R9: return {3, ESampleType::Float32, false, false, false};
        }

        return {};
    }
};

// Signed EAC is deliberately absent: etcdec decodes it with the unsigned formula and reinterprets the result, which does
// not match the spec's signed base codeword and clamping. Better to report it as unsupported than to show wrong values.
optional<EncodedLayout> encodedLayoutFromVkFormat(const uint32_t format) {
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return EncodedLayout{EEncoding::BC1, false, false};
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return EncodedLayout{EEncoding::BC1, true, false};
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return EncodedLayout{EEncoding::BC1A, false, false};
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return EncodedLayout{EEncoding::BC1A, true, false};
        case VK_FORMAT_BC2_UNORM_BLOCK: return EncodedLayout{EEncoding::BC2, false, false};
        case VK_FORMAT_BC2_SRGB_BLOCK: return EncodedLayout{EEncoding::BC2, true, false};
        case VK_FORMAT_BC3_UNORM_BLOCK: return EncodedLayout{EEncoding::BC3, false, false};
        case VK_FORMAT_BC3_SRGB_BLOCK: return EncodedLayout{EEncoding::BC3, true, false};
        case VK_FORMAT_BC4_UNORM_BLOCK: return EncodedLayout{EEncoding::BC4, false, false};
        case VK_FORMAT_BC4_SNORM_BLOCK: return EncodedLayout{EEncoding::BC4, false, true};
        case VK_FORMAT_BC5_UNORM_BLOCK: return EncodedLayout{EEncoding::BC5, false, false};
        case VK_FORMAT_BC5_SNORM_BLOCK: return EncodedLayout{EEncoding::BC5, false, true};
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: return EncodedLayout{EEncoding::BC6H, false, false};
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return EncodedLayout{EEncoding::BC6H, false, true};
        case VK_FORMAT_BC7_UNORM_BLOCK: return EncodedLayout{EEncoding::BC7, false, false};
        case VK_FORMAT_BC7_SRGB_BLOCK: return EncodedLayout{EEncoding::BC7, true, false};

        // KTX2 has no ETC1 format; ETC1 data is stored as ETC2 RGB, which is a superset.
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: return EncodedLayout{EEncoding::ETC2_RGB, false, false};
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK: return EncodedLayout{EEncoding::ETC2_RGB, true, false};
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: return EncodedLayout{EEncoding::ETC2_RGB_A1, false, false};
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK: return EncodedLayout{EEncoding::ETC2_RGB_A1, true, false};
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK: return EncodedLayout{EEncoding::ETC2_RGBA, false, false};
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK: return EncodedLayout{EEncoding::ETC2_RGBA, true, false};
        case VK_FORMAT_EAC_R11_UNORM_BLOCK: return EncodedLayout{EEncoding::EAC_R11, false, false};
        case VK_FORMAT_EAC_R11_SNORM_BLOCK: return EncodedLayout{EEncoding::EAC_R11, false, true};
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK: return EncodedLayout{EEncoding::EAC_RG11, false, false};
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK: return EncodedLayout{EEncoding::EAC_RG11, false, true};

        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return EncodedLayout{EEncoding::B10G11R11, false, false};
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return EncodedLayout{EEncoding::E5B9G9R9, false, false};

        default: return nullopt;
    }
}

optional<EncodedLayout> encodedLayoutFromGlInternalFormat(const uint32_t format) {
    switch (format) {
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: return EncodedLayout{EEncoding::BC1, false, false};
        case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT: return EncodedLayout{EEncoding::BC1, true, false};
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return EncodedLayout{EEncoding::BC1A, false, false};
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: return EncodedLayout{EEncoding::BC1A, true, false};
        case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return EncodedLayout{EEncoding::BC2, false, false};
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: return EncodedLayout{EEncoding::BC2, true, false};
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return EncodedLayout{EEncoding::BC3, false, false};
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return EncodedLayout{EEncoding::BC3, true, false};
        case GL_COMPRESSED_RED_RGTC1: return EncodedLayout{EEncoding::BC4, false, false};
        case GL_COMPRESSED_SIGNED_RED_RGTC1: return EncodedLayout{EEncoding::BC4, false, true};
        case GL_COMPRESSED_RG_RGTC2: return EncodedLayout{EEncoding::BC5, false, false};
        case GL_COMPRESSED_SIGNED_RG_RGTC2: return EncodedLayout{EEncoding::BC5, false, true};
        case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT: return EncodedLayout{EEncoding::BC6H, false, false};
        case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT: return EncodedLayout{EEncoding::BC6H, false, true};
        case GL_COMPRESSED_RGBA_BPTC_UNORM: return EncodedLayout{EEncoding::BC7, false, false};
        case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM: return EncodedLayout{EEncoding::BC7, true, false};

        case GL_ETC1_RGB8_OES:
        case GL_COMPRESSED_RGB8_ETC2: return EncodedLayout{EEncoding::ETC2_RGB, false, false};
        case GL_COMPRESSED_SRGB8_ETC2: return EncodedLayout{EEncoding::ETC2_RGB, true, false};
        case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2: return EncodedLayout{EEncoding::ETC2_RGB_A1, false, false};
        case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2: return EncodedLayout{EEncoding::ETC2_RGB_A1, true, false};
        case GL_COMPRESSED_RGBA8_ETC2_EAC: return EncodedLayout{EEncoding::ETC2_RGBA, false, false};
        case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC: return EncodedLayout{EEncoding::ETC2_RGBA, true, false};
        case GL_COMPRESSED_R11_EAC: return EncodedLayout{EEncoding::EAC_R11, false, false};
        case GL_COMPRESSED_SIGNED_R11_EAC: return EncodedLayout{EEncoding::EAC_R11, false, true};
        case GL_COMPRESSED_RG11_EAC: return EncodedLayout{EEncoding::EAC_RG11, false, false};
        case GL_COMPRESSED_SIGNED_RG11_EAC: return EncodedLayout{EEncoding::EAC_RG11, false, true};

        case GL_R11F_G11F_B10F: return EncodedLayout{EEncoding::B10G11R11, false, false};
        case GL_RGB9_E5: return EncodedLayout{EEncoding::E5B9G9R9, false, false};

        default: return nullopt;
    }
}

// The 11- and 10-bit unsigned floats share half's 5-bit exponent and lack a sign bit, so shifting the mantissa up to half's
// 10 bits yields a valid half bit pattern -- including denormals, infinity and NaN.
float unpackUFloat(const uint32_t bits, const int numMantissaBits) {
    half h;
    h.setBits((uint16_t)(bits << (10 - numMantissaBits)));
    return h;
}

void decodeBlock(const EncodedLayout& encoded, const uint8_t* block, uint8_t* dst, const int dstPitch) {
    switch (encoded.encoding) {
        case EEncoding::BC1:
        case EEncoding::BC1A: bcdec_bc1(block, dst, dstPitch); break;
        case EEncoding::BC2: bcdec_bc2(block, dst, dstPitch); break;
        case EEncoding::BC3: bcdec_bc3(block, dst, dstPitch); break;
        case EEncoding::BC4: bcdec_bc4_float(block, dst, dstPitch, encoded.isSigned); break;
        case EEncoding::BC5: bcdec_bc5_float(block, dst, dstPitch, encoded.isSigned); break;
        case EEncoding::BC6H: bcdec_bc6h_half(block, dst, dstPitch, encoded.isSigned); break;
        case EEncoding::BC7: bcdec_bc7(block, dst, dstPitch); break;

        case EEncoding::ETC2_RGB: etcdec_etc_rgb(block, dst, dstPitch); break;
        case EEncoding::ETC2_RGB_A1: etcdec_etc_rgb_a1(block, dst, dstPitch); break;
        case EEncoding::ETC2_RGBA: etcdec_eac_rgba(block, dst, dstPitch); break;
        case EEncoding::EAC_R11: etcdec_eac_r11_u16(block, dst, dstPitch); break;
        case EEncoding::EAC_RG11: etcdec_eac_rg11_u16(block, dst, dstPitch); break;

        case EEncoding::B10G11R11: {
            uint32_t v;
            memcpy(&v, block, sizeof(v));
            float* out = reinterpret_cast<float*>(dst);
            out[0] = unpackUFloat(v & 0x7FF, 6);
            out[1] = unpackUFloat((v >> 11) & 0x7FF, 6);
            out[2] = unpackUFloat(v >> 22, 5);
        } break;

        case EEncoding::E5B9G9R9: {
            uint32_t v;
            memcpy(&v, block, sizeof(v));
            // Shared exponent with bias 15, and 9-bit mantissas without an implicit leading one.
            const float scale = ldexp(1.0f, (int)(v >> 27) - 15 - 9);
            float* out = reinterpret_cast<float*>(dst);
            out[0] = (v & 0x1FF) * scale;
            out[1] = ((v >> 9) & 0x1FF) * scale;
            out[2] = ((v >> 18) & 0x1FF) * scale;
        } break;
    }
}

struct DecodedImage {
    vector<uint8_t> data;
    size_t rowPitch = 0;
};

// Decodes into a buffer padded to whole blocks. convertToFloat only reads the unpadded region, so edge blocks need no special care.
Task<DecodedImage> decode(const span<const uint8_t> src, const Vector2i& size, const EncodedLayout& encoded, const int priority) {
    const SampleLayout layout = encoded.decodedLayout();

    const int blockDim = encoded.blockDim();
    const int numBlocksX = (size.x() + blockDim - 1) / blockDim;
    const int numBlocksY = (size.y() + blockDim - 1) / blockDim;
    const size_t pixelBytes = layout.numChannels * layout.bytesPerSample();
    const size_t blockBytes = encoded.blockBytes();

    DecodedImage result;
    result.rowPitch = (size_t)numBlocksX * blockDim * pixelBytes;
    result.data.resize(result.rowPitch * numBlocksY * blockDim);

    const int dstPitch = (int)(result.rowPitch / encoded.pitchUnitBytes());

    co_await ThreadPool::global().parallelFor<int>(
        0,
        numBlocksY,
        (size_t)numBlocksX * numBlocksY * blockDim * blockDim * layout.numChannels,
        [&](int by) {
            for (int bx = 0; bx < numBlocksX; ++bx) {
                // etcdec reads blocks through unsigned long long*, but KTX1 image offsets are only guaranteed to be 4-byte aligned.
                alignas(16) uint8_t block[16];
                memcpy(block, &src[((size_t)by * numBlocksX + bx) * blockBytes], blockBytes);

                uint8_t* dst = &result.data[(size_t)by * blockDim * result.rowPitch + (size_t)bx * blockDim * pixelBytes];
                decodeBlock(encoded, block, dst, dstPitch);
            }
        },
        priority
    );

    co_return result;
}

struct KtxTextureDeleter {
    void operator()(ktxTexture* texture) const {
        if (texture) {
            ktxTexture_Destroy(texture);
        }
    }
};

using KtxTexturePtr = unique_ptr<ktxTexture, KtxTextureDeleter>;

// KTX asks for NUL-terminated metadata strings, but not every writer emits the terminator.
string_view readStringValue(ktxTexture* texture, const char* key) {
    ktx_uint32_t valueLen = 0;
    char* value = nullptr;

    if (ktxHashList_FindValue(&texture->kvDataHead, key, &valueLen, (void**)&value) != KTX_SUCCESS || !value) {
        return {};
    }

    string_view result{value, valueLen};
    while (!result.empty() && result.back() == '\0') {
        result.remove_suffix(1);
    }

    return result;
}

// KTX stores orientation in the KTXorientation metadata key. KTX1 spells it "S=r,T=d", KTX2 "rd". The GL-flavored default
// is bottom-up, so a missing or "T=u"/"ru" value means we have to flip.
bool needsVerticalFlip(ktxTexture* texture) {
    const string_view orientation = readStringValue(texture, KTX_ORIENTATION_KEY);
    return orientation.empty() ? texture->classId == ktxTexture1_c : orientation.find('u') != string_view::npos;
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
    const size_t srcStride = layout.numChannels;
    const size_t numChannels = std::min(layout.numOutputChannels(), dst.nChannels());
    const bool srgb = layout.srgb;
    // Only the color channels of a BGR(A) image are swizzled; alpha stays put.
    const size_t numSwizzled = layout.bgr ? std::min(numChannels, 3uz) : 0uz;
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
                    const size_t srcC = c < numSwizzled ? numSwizzled - 1 - c : c;
                    float value = normalize<T>(srcRow[x * srcStride + srcC]);

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

    // KTX2 payloads may need decoding before we can look at them as plain samples. Supercompression (Zstd/ZLIB) was already undone
    // by the LOAD_IMAGE_DATA flag above; what remains is Basis and ASTC.
    if (texture->classId == ktxTexture2_c) {
        ktxTexture2* texture2 = reinterpret_cast<ktxTexture2*>(texture.get());

        premultipliedAlpha = ktxTexture2_GetPremultipliedAlpha(texture2);

        if (ktxTexture2_NeedsTranscoding(texture2)) {
            // BasisLZ/ETC1S and UASTC. UASTC HDR would clamp in 8-bit RGBA32, so it gets a half-float target instead. Both land as
            // plain VkFormats handled below.
            const ktx_transcode_fmt_e target = ktxTexture2_IsHDR(texture2) ? KTX_TTF_RGB_HALF : KTX_TTF_RGBA32;
            if (const KTX_error_code err = ktxTexture2_TranscodeBasis(texture2, target, 0); err != KTX_SUCCESS) {
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
    optional<EncodedLayout> encoded;
    string formatName;

    if (texture->classId == ktxTexture2_c) {
        const uint32_t vkFormat = reinterpret_cast<ktxTexture2*>(texture.get())->vkFormat;
        layout = layoutFromVkFormat(vkFormat);
        encoded = encodedLayoutFromVkFormat(vkFormat);
        formatName = fmt::format("VkFormat {}", vkFormat);
    } else {
        const uint32_t glInternalFormat = reinterpret_cast<ktxTexture1*>(texture.get())->glInternalformat;
        layout = layoutFromGlInternalFormat(glInternalFormat);
        encoded = encodedLayoutFromGlInternalFormat(glInternalFormat);
        formatName = fmt::format("GL internal format {:#x}", glInternalFormat);
    }

    if (encoded) {
        // Decoded into an intermediate buffer first, after which it's converted like any other image.
        layout = encoded->decodedLayout();
    } else if (!layout || texture->isCompressed) {
        throw ImageLoadError{fmt::format("Unsupported KTX pixel format ({}).", formatName)};
    }

    // KTXswizzle maps output RGBA to source components. KTX2 has no luminance formats, so a two-channel format whose alpha is
    // sourced from G is luminance-alpha. This applies to BC5 and EAC RG11 as much as to uncompressed RG.
    if (const string_view swizzle = readStringValue(texture.get(), KTX_SWIZZLE_KEY);
        layout->numChannels == 2 && swizzle.size() == 4 && swizzle[3] == 'g') {
        layout->alpha = true;
    }

    tlog::debug(
        "ktx image: size={} format={} channels={} alpha={} premultipliedAlpha={}",
        Vector3i{(int)texture->baseWidth, (int)texture->baseHeight, (int)texture->baseDepth},
        formatName,
        layout->numOutputChannels(),
        layout->alpha,
        premultipliedAlpha
    );

    const bool flipY = needsVerticalFlip(texture.get());

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

                    // For compressed formats ktxTexture_GetRowPitch is the pitch of a row of *blocks*, so rowPitch * height would overshoot.
                    const size_t imageSize = encoded ? encoded->encodedSize(levelSize) : rowPitch * levelSize.y();
                    if (offset + imageSize > data.size()) {
                        throw ImageLoadError{"KTX subimage extends past the end of the payload."};
                    }

                    ImageData& resultData = result.emplace_back();
                    resultData.channels = co_await makeInterleavedChannels(
                        layout->numOutputChannels(),
                        layout->alpha,
                        levelSize,
                        EPixelFormat::F32,
                        layout->type == ESampleType::Float32 ? EPixelFormat::F32 : EPixelFormat::F16,
                        "",
                        priority
                    );

                    resultData.hasPremultipliedAlpha = layout->alpha && premultipliedAlpha;
                    resultData.partName = makePartName(texture.get(), level, layer, face, slice);

                    const auto dst = MultiChannelView<float>{resultData.channels};
                    span<const uint8_t> src = data.subspan(offset, imageSize);
                    size_t srcRowPitch = rowPitch;

                    DecodedImage decoded;
                    if (encoded) {
                        decoded = co_await decode(src, levelSize, *encoded, priority);
                        src = decoded.data;
                        srcRowPitch = decoded.rowPitch;
                    }

                    switch (layout->type) {
                        case ESampleType::UNorm8: co_await convertToFloat<uint8_t>(src, srcRowPitch, dst, *layout, flipY, priority); break;
                        case ESampleType::UNorm16:
                            co_await convertToFloat<uint16_t>(src, srcRowPitch, dst, *layout, flipY, priority);
                            break;
                        case ESampleType::Float16: co_await convertToFloat<half>(src, srcRowPitch, dst, *layout, flipY, priority); break;
                        case ESampleType::Float32: co_await convertToFloat<float>(src, srcRowPitch, dst, *layout, flipY, priority); break;
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
