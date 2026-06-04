/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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

#include <tev/imageio/Demosaic.h>
#include <tev/imageio/FitsImageLoader.h>

#include <fitsio.h>

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

namespace {

// Throw an ImageLoadError if cfitsio reported a non-OK status.
void throwOnCfitsioError(int status, string_view context) {
    if (status == 0) {
        return;
    }

    char msg[FLEN_ERRMSG] = {};
    fits_get_errstatus(status, msg);
    throw ImageLoadError{fmt::format("cfitsio error during {} (status {}): {}", context, status, msg)};
}

// Strip surrounding apostrophes from a FITS string-value field if present, trim trailing spaces inside the quotes, and unescape doubled
// apostrophes.
string stripValueQuotes(const char* raw) {
    if (!raw || !*raw) {
        return {};
    }

    string_view s{raw};
    if (s.size() < 2 || s.front() != '\'' || s.back() != '\'') {
        return string{s}; // numeric or logical value; pass through unchanged.
    }

    s = s.substr(1, s.size() - 2);
    while (!s.empty() && s.back() == ' ') {
        s = s.substr(0, s.size() - 1);
    }

    // Collapse '' to '.
    ostringstream out;
    for (size_t i = 0; i < s.size(); ++i) {
        out << s[i];
        if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
            ++i; // skip the second apostrophe of the pair
        }
    }

    return out.str();
}

// Read the EXTNAME keyword from the current HDU.
optional<string> readExtname(fitsfile* fp) {
    char extname[FLEN_VALUE] = {};
    int status = 0;
    fits_read_key(fp, TSTRING, "EXTNAME", extname, nullptr, &status);
    if (status != 0) {
        return nullopt;
    }

    string_view trimmed = trim(extname);
    if (trimmed.empty()) {
        return nullopt;
    }

    return string{trimmed};
}

// Decode the image HDU at the current cfitsio cursor position into ImageData. Returns nullopt if the HDU is image-like but is not loadable
// (NAXIS not in {2,3}, NAXIS1=0 (Random Groups), or zero pixels).
Task<optional<ImageData>> decodeImageHdu(fitsfile* fp, int hduIndex, int priority) {
    int status = 0;

    int naxis = 0;
    fits_get_img_dim(fp, &naxis, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_dim", hduIndex));
    if (naxis != 2 && naxis != 3) {
        co_return nullopt;
    }

    long axes[3] = {0};
    fits_get_img_size(fp, naxis, axes, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_size", hduIndex));

    // Reject zero-extent axes. NAXIS1=0 is the Random Groups sentinel.
    if (axes[0] <= 0 || axes[1] <= 0) {
        co_return nullopt;
    }

    const Vector2i size{(int)axes[0], (int)axes[1]};
    const size_t numPixels = (size_t)axes[0] * (size_t)axes[1];
    const size_t numChannels = (naxis == 3) ? (size_t)axes[2] : 1;
    if (numChannels == 0) {
        co_return nullopt;
    }

    int bitpix = 0;
    fits_get_img_type(fp, &bitpix, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_type", hduIndex));
    if (bitpix == LONGLONG_IMG) {
        tlog::warning("HDU {} has BITPIX=64; precision will be lost converting to float32.", hduIndex);
    }

    // Negative values indigate floating-point types, positive values integers. For integers, ~lossless fp16 representation means fitting
    // into the 10-bit mantissa.
    const bool fitsIntoFloat16 = bitpix <= 10 && bitpix >= -16;

    const auto channelName = [numChannels](size_t c) -> string {
        if (numChannels == 1) {
            return "L";
        } else if (numChannels == 3) {
            static const string rgbNames[] = {"R", "G", "B"};
            return rgbNames[c];
        } else {
            return to_string(c);
        }
    };

    ImageData resultData;

    const auto extname = readExtname(fp);
    resultData.partName = extname ? fmt::format("hdu.{}", *extname) : fmt::format("hdu.{}", hduIndex);

    // Build channels by reading each plane directly into its float channel storage. cfitsio applies BSCALE/BZERO and any unsigned-integer
    // conversion when reading as TFLOAT, so we always get float32 physical units.
    for (size_t c = 0; c < numChannels; ++c) {
        auto& channel = resultData.channels.emplace_back(
            channelName(c), size, EPixelFormat::F32, fitsIntoFloat16 ? EPixelFormat::F16 : EPixelFormat::F32
        );

        LONGLONG fpixel[3] = {1, 1, (LONGLONG)c + 1};
        float nullval = 0.0f;
        int anynull = 0;
        fits_read_pixll(fp, TFLOAT, fpixel, (LONGLONG)numPixels, &nullval, channel.view<float>().data(), &anynull, &status);

        throwOnCfitsioError(status, fmt::format("HDU {} read_pixll plane {}", hduIndex, c));
    }

    resultData.hasPremultipliedAlpha = true;
    resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;

    // Copy FITS header keywords for this HDU to resultData's attributes
    int nKeys = 0;
    fits_get_hdrspace(fp, &nKeys, nullptr, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_hdrspace", hduIndex));

    AttributeNode& root = resultData.attributes.emplace_back();
    root.name = "FITS header";
    AttributeNode& section = root.children.emplace_back();
    section.name = "";

    // FITS images are conventionally stored in bottom-up orientation, but this can be overridden by the optional ROWORDER keyword.
    resultData.orientation = EOrientation::BottomLeft;

    unordered_map<string, string> headerVals;

    for (int k = 1; k <= nKeys; ++k) {
        char key[FLEN_KEYWORD] = {}, value[FLEN_VALUE] = {}, comment[FLEN_COMMENT] = {};
        int keyStatus = 0;

        fits_read_keyn(fp, k, key, value, comment, &keyStatus);
        if (keyStatus != 0) {
            continue;
        }

        const string valueStr = stripValueQuotes(value);
        headerVals[key] = valueStr;

        AttributeNode& node = section.children.emplace_back();
        node.name = key;
        node.value = std::move(valueStr);
        node.type = comment;
    }

    // CBLACK and CWHITE are display hints that, while not part of the FITS standard and not strictly speaking affecting the underlying
    // data, are common enough in practice that we will rescale if they're present.
    float cwhite = 1.0f;
    float cblack = 0.0f;

    if (auto it = headerVals.find("CWHITE"); it != headerVals.end()) {
        fromChars(it->second, cwhite);
    }

    if (auto it = headerVals.find("CBLACK"); it != headerVals.end()) {
        fromChars(it->second, cblack);
    }

    if (cwhite != 1.0f || cblack != 0.0f) {
        tlog::debug("HDU {} has CWHITE={} and CBLACK={}. Rescaling.", hduIndex, cwhite, cblack);

        const float scale = 1.0f / (cwhite - cblack);
        for (size_t c = 0; c < numChannels; ++c) {
            co_await ThreadPool::global().parallelFor(
                0uz,
                numPixels,
                numPixels,
                [view = resultData.channels[c].view<float>(), scale, cblack](size_t i) { view[i] = (view[i] - cblack) * scale; },
                priority
            );
        }
    }

    // ROWORDER and BAYERPAT are implemented following https://siril.readthedocs.io/en/latest/file-formats/FITS.html#retrieving-the-bayer-matrix
    // such that the sample images on that page are correctly debayered and rendered in the correct orientation.
    if (auto it = headerVals.find("ROWORDER"); it != headerVals.end()) {
        const auto& rowOrder = it->second;
        if (rowOrder != "BOTTOM-UP" && rowOrder != "TOP-DOWN") {
            tlog::warning("HDU {} has unrecognized ROWORDER value '{}'; ignoring.", hduIndex, rowOrder);
        } else {
            resultData.orientation = rowOrder == "BOTTOM-UP" ? EOrientation::TopLeft : EOrientation::BottomLeft;
        }
    }

    const auto applyBayerPattern = [&](string bayerPatStr) -> Task<void> {
        if (numChannels != 1) {
            tlog::warning("HDU {} has BAYERPAT or COLORTYP keyword but multiple channels; skipping debayering.", hduIndex, numChannels);
            co_return;
        }

        if (bayerPatStr != "RGGB" && bayerPatStr != "BGGR" && bayerPatStr != "GRBG" && bayerPatStr != "GBRG") {
            tlog::warning("HDU {} has unrecognized BAYERPAT value '{}'; assuming RGGB.", hduIndex, bayerPatStr);
            bayerPatStr = "RGGB";
        }

        Vector2i bayerOffset = {0, 0};
        if (auto xoffIt = headerVals.find("XBAYROFF"); xoffIt != headerVals.end()) {
            if (!fromChars(xoffIt->second, bayerOffset.x())) {
                tlog::warning("HDU {} has invalid BAYERXOFF value '{}'; using default 0.", hduIndex, xoffIt->second);
                bayerOffset.x() = 0;
            }
        }

        if (auto yoffIt = headerVals.find("YBAYROFF"); yoffIt != headerVals.end()) {
            if (!fromChars(yoffIt->second, bayerOffset.y())) {
                tlog::warning("HDU {} has invalid BAYERXOFF value '{}'; using default 0.", hduIndex, yoffIt->second);
                bayerOffset.y() = 0;
            }
        }

        tlog::debug("HDU {} has BAYERPAT='{}' with offset ({}, {}). Applying.", hduIndex, bayerPatStr, bayerOffset.x(), bayerOffset.y());

        const int bayerWidth = sqrt((int)bayerPatStr.size());
        if (bayerWidth * bayerWidth != (int)bayerPatStr.size()) {
            tlog::warning("HDU {} has BAYERPAT of length {} that is not a perfect square; ignoring.", hduIndex, bayerPatStr.size());
            co_return;
        }

        vector<uint8_t> bayerpat;
        for (int y = 0; y < bayerWidth; ++y) {
            for (int x = 0; x < bayerWidth; ++x) {
                const auto xy = Vector2i{(x + bayerOffset.x()) % bayerWidth, bayerWidth - 1 - ((y + bayerOffset.y()) % bayerWidth)};
                const auto reorientedXy = applyOrientation(resultData.orientation, xy, Vector2i{bayerWidth});

                const auto idx = (size_t)(reorientedXy.y() * bayerWidth + reorientedXy.x());
                const char c = toupper(bayerPatStr[idx]);

                TEV_ASSERT(c == 'R' || c == 'G' || c == 'B', "BAYERPAT must only contain R, G, and B characters");
                bayerpat.emplace_back(c == 'R' ? 0 : (c == 'G' ? 1 : 2));
            }
        }

        TEV_ASSERT(numChannels > 0, "Unexpected zero channels after earlier check");
        const auto numInterleavedChannels = nextSupportedTextureChannelCount(3);
        auto rgbaChannels = co_await ImageLoader::makeInterleavedChannels(
            3, numInterleavedChannels, false, size, EPixelFormat::F32, resultData.channels.front().desiredPixelFormat(), "", priority
        );

        co_await demosaic(
            resultData.channels.front().view<float>(), MultiChannelView<float>{rgbaChannels}, bayerpat, Vector2i{bayerWidth}, priority
        );

        resultData.channels.front().setName("cfa.L");
        resultData.channels.insert(
            resultData.channels.begin(), make_move_iterator(rgbaChannels.begin()), make_move_iterator(rgbaChannels.end())
        );
    };

    if (auto it = headerVals.find("BAYERPAT"); it != headerVals.end()) {
        co_await applyBayerPattern(it->second);
    } else if (it = headerVals.find("COLORTYP"); it != headerVals.end()) {
        co_await applyBayerPattern(it->second);
    }

    co_return resultData;
}

} // anonymous namespace

Task<vector<ImageData>>
    FitsImageLoader::load(istream& iStream, const fs::path& path, string_view, const ImageLoaderSettings&, int priority) const {
    char magic[9] = {};
    iStream.read(magic, sizeof(magic));
    if (!iStream || string_view{magic, sizeof(magic)} != "SIMPLE  =") {
        throw FormatNotSupported{"File is not a FITS image."};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    const auto dataSize = iStream.tellg();
    iStream.seekg(0, iStream.beg);

    HeapArray<char> data(dataSize);
    iStream.read(data.data(), dataSize);
    if (!iStream) {
        throw ImageLoadError{fmt::format("Failed to read FITS data of size {}", (size_t)dataSize)};
    }

    fitsfile* fp = nullptr;
    int status = 0;
    void* bufferPtr = data.data();
    size_t bufferSize = (size_t)dataSize;
    const string memName = fmt::format("{}.mem", path.filename().string());
    fits_open_memfile(&fp, memName.c_str(), READONLY, &bufferPtr, &bufferSize, 0, nullptr, &status);
    throwOnCfitsioError(status, "open_memfile");

    const auto fitsGuard = ScopeGuard{[&]() {
        int closeStatus = 0;
        fits_close_file(fp, &closeStatus);
    }};

    int numHdus = 0;
    fits_get_num_hdus(fp, &numHdus, &status);
    throwOnCfitsioError(status, "get_num_hdus");

    vector<ImageData> result;
    int skippedNonImage = 0;

    // cfitsio HDU indexing is 1-based
    for (int i = 1; i <= numHdus; ++i) {
        int hduType = 0;
        fits_movabs_hdu(fp, i, &hduType, &status);
        throwOnCfitsioError(status, fmt::format("HDU {} movabs_hdu", i));
        if (hduType != IMAGE_HDU) {
            ++skippedNonImage;
            continue;
        }

        if (auto imageData = co_await decodeImageHdu(fp, i, priority)) {
            result.emplace_back(std::move(*imageData));
        }
    }

    if (skippedNonImage > 0) {
        tlog::warning("FITS file: skipped {} non-image {}.", skippedNonImage, skippedNonImage == 1 ? "HDU" : "HDUs");
    }

    if (result.empty()) {
        throw ImageLoadError{"No loadable image HDUs found in FITS file."};
    }

    if (result.size() == 1) {
        // Don't use a part name if there was only a single part
        result.front().partName.clear();
    }

    co_return result;
}

} // namespace tev
