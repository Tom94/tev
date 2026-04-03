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

#include <tev/imageio/FitsImageLoader.h>

#include <fitsio.h>

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

// Strip surrounding apostrophes from a FITS string-value field if present,
// trim trailing spaces inside the quotes, and unescape doubled apostrophes.
string stripValueQuotes(const char* raw) {
    if (!raw || !*raw) {
        return {};
    }
    string s{raw};
    if (s.size() < 2 || s.front() != '\'' || s.back() != '\'') {
        return s; // numeric or logical value; pass through unchanged.
    }
    s = s.substr(1, s.size() - 2);
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    // Collapse '' to '.
    string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(s[i]);
        if (s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') {
            ++i; // skip the second apostrophe of the pair
        }
    }
    return out;
}

// Decode the image HDU at the current cfitsio cursor position into ImageData.
// Returns nullopt if the HDU is image-like but is not loadable
// (NAXIS not in {2,3}, NAXIS1=0 (Random Groups), or zero pixels).
optional<ImageData> decodeImageHdu(fitsfile* fp, int hduIndex) {
    int status = 0;

    int naxis = 0;
    fits_get_img_dim(fp, &naxis, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_dim", hduIndex));
    if (naxis != 2 && naxis != 3) {
        return nullopt;
    }

    long axes[3] = {0};
    fits_get_img_size(fp, naxis, axes, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_size", hduIndex));

    // Reject zero-extent axes. NAXIS1=0 is the Random Groups sentinel.
    if (axes[0] <= 0 || axes[1] <= 0) {
        return nullopt;
    }

    const Vector2i size{(int)axes[0], (int)axes[1]};
    const size_t numPixels = (size_t)axes[0] * (size_t)axes[1];
    const size_t numChannels = (naxis == 3) ? (size_t)axes[2] : 1;
    if (numChannels == 0) {
        return nullopt;
    }

    int bitpix = 0;
    fits_get_img_type(fp, &bitpix, &status);
    throwOnCfitsioError(status, fmt::format("HDU {} get_img_type", hduIndex));
    if (bitpix == LONGLONG_IMG) {
        tlog::warning("HDU {} has BITPIX=64; precision will be lost converting to float32.", hduIndex);
    }

    ImageData resultData;

    // Build channels by reading each plane directly into its float channel storage.
    // cfitsio applies BSCALE/BZERO and any unsigned-integer conversion when
    // reading as TFLOAT, so we always get float32 physical units.
    const string_view rgbNames[] = {"R", "G", "B"};
    LONGLONG fpixel[3] = {1, 1, 1};
    float nullval = 0.0f;
    int anynull = 0;
    for (size_t c = 0; c < numChannels; ++c) {
        string name;
        if (numChannels == 1) {
            name = "L";
        } else if (numChannels == 3) {
            name = rgbNames[c];
        } else {
            name = to_string(c);
        }

        resultData.channels.emplace_back(name, size, EPixelFormat::F32, EPixelFormat::F32);
        fpixel[2] = (LONGLONG)(c + 1);
        fits_read_pixll(
            fp, TFLOAT, fpixel, (LONGLONG)numPixels, &nullval, resultData.channels[c].view<float>().data(), &anynull, &status
        );
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
    for (int k = 1; k <= nKeys; ++k) {
        char key[FLEN_KEYWORD] = {}, value[FLEN_VALUE] = {}, comment[FLEN_COMMENT] = {};
        int keyStatus = 0;
        fits_read_keyn(fp, k, key, value, comment, &keyStatus);
        if (keyStatus != 0) {
            continue;
        }
        AttributeNode& node = section.children.emplace_back();
        node.name = key;
        node.value = stripValueQuotes(value);
        node.type = comment;
    }

    return resultData;
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

} // anonymous namespace

Task<vector<ImageData>> FitsImageLoader::load( istream& iStream, const fs::path& path, string_view, const ImageLoaderSettings&, int priority) const {
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

    auto fitsGuard = ScopeGuard{[&]() {
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
        auto extname = readExtname(fp);
        auto imageData = decodeImageHdu(fp, i);
        if (imageData) {
            imageData->partName = extname ? fmt::format("hdu.{}", *extname) : fmt::format("hdu.{}", i);
            result.emplace_back(std::move(*imageData));
        }
    }

    if (skippedNonImage > 0) {
        tlog::info("FITS file: skipped {} non-image {}.", skippedNonImage, skippedNonImage == 1 ? "HDU" : "HDUs");
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
