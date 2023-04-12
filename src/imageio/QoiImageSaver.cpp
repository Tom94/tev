// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/QoiImageSaver.h>

#define QOI_NO_STDIO
#include <qoi.h>

#include <ostream>
#include <vector>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

void QoiImageSaver::save(ostream& oStream, const fs::path&, const vector<char>& data, const Vector2i& imageSize, int nChannels) const {
    // The QOI image format expects nChannels to be either 3 for RGB data or 4 for RGBA.
    if (nChannels != 4 && nChannels != 3) {
        throw invalid_argument{format("Invalid number of channels {}.", nChannels)};
    }

    const qoi_desc desc{
        .width = static_cast<unsigned int>(imageSize.x()),
        .height = static_cast<unsigned int>(imageSize.y()),
        .channels = static_cast<unsigned char>(nChannels),
        .colorspace = QOI_SRGB,
    };
    int sizeInBytes = 0;
    void *encodedData = qoi_encode(data.data(), &desc, &sizeInBytes);

    ScopeGuard encodedDataGuard{[encodedData] { free(encodedData); }};

    if (!encodedData) {
        throw invalid_argument{"Failed to encode data into the QOI format."};
    }

    oStream.write(reinterpret_cast<char*>(encodedData), sizeInBytes);
}

TEV_NAMESPACE_END
