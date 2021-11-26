// This file was developed by Tiago Chaves & Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/QoiImageSaver.h>

#include <qoi.h>

#include <ostream>
#include <vector>

using namespace Eigen;
using namespace filesystem;
using namespace std;

TEV_NAMESPACE_BEGIN

// FIXME: this is using `path` directly (and ignoring `oStream`).
void QoiImageSaver::save(ostream& oStream, const path& path, const vector<char>& data, const Vector2i& imageSize, int nChannels) const {
    // The QOI data format expects nChannels to be either 3 for RGB data or 4 for RGBA.
    qoi_write(path.str().c_str(), data.data(), imageSize.x(), imageSize.y(), nChannels);
}

TEV_NAMESPACE_END