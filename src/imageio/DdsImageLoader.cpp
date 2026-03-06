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
#include <tev/ThreadPool.h>
#include <tev/imageio/DdsImageLoader.h>

#include <DirectXTex.h>

using namespace nanogui;
using namespace std;

namespace tev {

static size_t getDxgiChannelCount(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM: return 4;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_AYUV:
        case DXGI_FORMAT_Y410:
        case DXGI_FORMAT_Y416:
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
        case DXGI_FORMAT_420_OPAQUE:
        case DXGI_FORMAT_YUY2:
        case DXGI_FORMAT_Y210:
        case DXGI_FORMAT_Y216:
        case DXGI_FORMAT_NV11:
        case DXGI_FORMAT_AI44:
        case DXGI_FORMAT_IA44:
        case DXGI_FORMAT_P8:
        case DXGI_FORMAT_A8P8:
        case DXGI_FORMAT_P208:
        case DXGI_FORMAT_V208:
        case DXGI_FORMAT_V408: return 3;
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return 2;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R1_UNORM:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return 1;
        case DXGI_FORMAT_UNKNOWN:
        default: return 0;
    }
}

Task<vector<ImageData>> DdsImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    iStream.seekg(0, iStream.end);
    const size_t dataSize = iStream.tellg();
    if (dataSize < 4) {
        throw FormatNotSupported{"File is too small."};
    }

    iStream.clear();
    iStream.seekg(0);

    HeapArray<char> data{dataSize};
    iStream.read(data.data(), 4);
    if (data[0] != 'D' || data[1] != 'D' || data[2] != 'S' || data[3] != ' ') {
        throw FormatNotSupported{"File is not a DDS file."};
    }

    iStream.read(data.data() + 4, dataSize - 4);

    // COM must be initialized on the thread executing the following DirectX calls. Thus: when editing this file *make sure* that no
    // co_await calls are made before the last DirectX call! Note that it is not a problem that CoInitializeEx will potentially get called
    // multiple times across different threads, or even multiple times by the same thread pool thread. Both situations are explicitly
    // allowed (and in the latter case, the return value is S_FALSE). See
    // https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex
    if (const auto res = CoInitializeEx(nullptr, COINIT_MULTITHREADED); res != S_OK && res != S_FALSE) {
        throw ImageLoadError{"Failed to initialize COM."};
    }

    // The correct way to clean up would be to call CoUninitialize() on every task pool thread on shutdown. Using a scope guard here
    // wouldn't work for multiple reasons: (i) the coroutine might have been scheduled to another thread once the scope ends, and (ii) it
    // would be wasteful to repeatedly initialize and uninitialize COM for every loaded image. Instead, we'll just accept that COM won't be
    // gracefully cleaned up on shutdown -- we don't care much about its cleanup anyway, which involved mostly handling of pending messages
    // in the application's message queue.
    // const auto comScopeGuard = ScopeGuard{[]() { CoUninitialize(); }};

    DirectX::ScratchImage scratchImage;
    DirectX::TexMetadata metadata;
    if (DirectX::LoadFromDDSMemory(data.data(), dataSize, DirectX::DDS_FLAGS_NONE, &metadata, scratchImage) != S_OK) {
        throw ImageLoadError{"Failed to read DDS file."};
    }

    DXGI_FORMAT format;
    const size_t numChannels = getDxgiChannelCount(metadata.format);
    switch (numChannels) {
        case 4: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case 3: format = DXGI_FORMAT_R32G32B32_FLOAT; break;
        case 2: format = DXGI_FORMAT_R32G32_FLOAT; break;
        case 1: format = DXGI_FORMAT_R32_FLOAT; break;
        default: throw ImageLoadError{std::format("Unsupported DXGI format: {}", static_cast<int>(metadata.format))};
    }

    // Use DirectXTex to either decompress or convert to the target floating point format.
    if (DirectX::IsCompressed(metadata.format)) {
        DirectX::ScratchImage decompImage;
        if (DirectX::Decompress(*scratchImage.GetImage(0, 0, 0), format, decompImage) != S_OK) {
            throw ImageLoadError{"Failed to decompress DDS image."};
        }

        swap(scratchImage, decompImage);
    } else if (metadata.format != format) {
        DirectX::ScratchImage convertedImage;
        if (DirectX::Convert(
                *scratchImage.GetImage(0, 0, 0), format, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, convertedImage
            ) != S_OK) {
            throw ImageLoadError{"Failed to convert DDS image."};
        }

        swap(scratchImage, convertedImage);
    }

    const Vector2i size = {(int)metadata.width, (int)metadata.height};

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    resultData.hasPremultipliedAlpha = scratchImage.GetMetadata().IsPMAlpha();

    const size_t numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);
    const bool hasAlpha = DirectX::HasAlpha(metadata.format);

    resultData.channels = co_await makeRgbaInterleavedChannels(
        numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F32, "", priority
    );

    const auto outView = MultiChannelView<float>{resultData.channels};

    const auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw ImageLoadError{"DDS image has zero pixels."};
    }

    const auto numSamples = numPixels * numChannels;

    const bool isFloat = DirectX::FormatDataType(metadata.format) == DirectX::FORMAT_TYPE_FLOAT;
    const auto s = span{(const float*)scratchImage.GetPixels(), numSamples};

    if (isFloat || numChannels < 3) {
        TEV_ASSERT(
            !DirectX::IsSRGB(metadata.format),
            "DXGI format {} is in sRGB space, but has a floating point data type.",
            static_cast<int>(metadata.format)
        );

        co_await toFloat32(s, numChannels, outView, hasAlpha, priority);
    } else {
        // Ideally, we'd be able to assume that only *_SRGB format images were in sRGB space, and only they need to converted to linear.
        // However, RGB(A) DDS images tend to be in sRGB space, even those not explicitly stored in an *_SRGB format.
        co_await toFloat32<float, true>(s, numChannels, outView, hasAlpha, priority);
    }

    co_return result;
}

} // namespace tev
