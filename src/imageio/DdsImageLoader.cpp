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
#include <tev/imageio/DdsImageLoader.h>

#include <DirectXTex.h>

using namespace nanogui;
using namespace std;

namespace tev {

static int getDxgiChannelCount(DXGI_FORMAT fmt) {
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

Task<vector<ImageData>> DdsImageLoader::load(istream& iStream, const fs::path&, const string&, int priority, bool) const {
    iStream.seekg(0, iStream.end);
    size_t dataSize = iStream.tellg();
    if (dataSize < 4) {
        throw FormatNotSupportedException{"File is too small."};
    }

    iStream.clear();
    iStream.seekg(0);
    vector<char> data(dataSize);
    iStream.read(data.data(), 4);
    if (data[0] != 'D' || data[1] != 'D' || data[2] != 'S' || data[3] != ' ') {
        throw FormatNotSupportedException{"File is not a DDS file."};
    }

    iStream.read(data.data() + 4, dataSize - 4);

    // COM must be initialized on the thread executing load().
    if (CoInitializeEx(nullptr, COINIT_MULTITHREADED) != S_OK) {
        throw invalid_argument{"Failed to initialize COM."};
    }

    ScopeGuard comScopeGuard{[]() { CoUninitialize(); }};

    DirectX::ScratchImage scratchImage;
    DirectX::TexMetadata metadata;
    if (DirectX::LoadFromDDSMemory(data.data(), dataSize, DirectX::DDS_FLAGS_NONE, &metadata, scratchImage) != S_OK) {
        throw invalid_argument{"Failed to read DDS file."};
    }

    DXGI_FORMAT format;
    int numChannels = getDxgiChannelCount(metadata.format);
    switch (numChannels) {
        case 4: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case 3: format = DXGI_FORMAT_R32G32B32_FLOAT; break;
        case 2: format = DXGI_FORMAT_R32G32_FLOAT; break;
        case 1: format = DXGI_FORMAT_R32_FLOAT; break;
        case 0:
        default: throw invalid_argument{fmt::format("Unsupported DXGI format: {}", static_cast<int>(metadata.format))};
    }

    // Use DirectXTex to either decompress or convert to the target floating point format.
    if (DirectX::IsCompressed(metadata.format)) {
        DirectX::ScratchImage decompImage;
        if (DirectX::Decompress(*scratchImage.GetImage(0, 0, 0), format, decompImage) != S_OK) {
            throw invalid_argument{"Failed to decompress DDS image."};
        }
        std::swap(scratchImage, decompImage);
    } else if (metadata.format != format) {
        DirectX::ScratchImage convertedImage;
        if (DirectX::Convert(
                *scratchImage.GetImage(0, 0, 0), format, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, convertedImage
            ) != S_OK) {
            throw invalid_argument{"Failed to convert DDS image."};
        }
        std::swap(scratchImage, convertedImage);
    }

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    resultData.channels = makeNChannels(numChannels, {(int)metadata.width, (int)metadata.height});

    auto numPixels = (size_t)metadata.width * metadata.height;
    if (numPixels == 0) {
        throw invalid_argument{"DDS image has zero pixels."};
    }

    bool isFloat = DirectX::FormatDataType(metadata.format) == DirectX::FORMAT_TYPE_FLOAT;

    if (isFloat || numChannels < 3) {
        assert(!DirectX::IsSRGB(metadata.format));
        // Assume that the image data is already in linear space.
        auto typedData = reinterpret_cast<float*>(scratchImage.GetPixels());
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    resultData.channels[c].at(i) = typedData[baseIdx + c];
                }
            },
            priority
        );
    } else {
        // Ideally, we'd be able to assume that only *_SRGB format images were in sRGB space, and only they need to converted to linear.
        // However, RGB(A) DDS images tend to be in sRGB space, even those not explicitly stored in an *_SRGB format.
        auto typedData = reinterpret_cast<float*>(scratchImage.GetPixels());
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                size_t baseIdx = i * numChannels;
                for (int c = 0; c < numChannels; ++c) {
                    if (c == 3) {
                        resultData.channels[c].at(i) = typedData[baseIdx + c];
                    } else {
                        resultData.channels[c].at(i) = toLinear(typedData[baseIdx + c]);
                    }
                }
            },
            priority
        );
    }

    resultData.hasPremultipliedAlpha = scratchImage.GetMetadata().IsPMAlpha();

    co_return result;
}

} // namespace tev
