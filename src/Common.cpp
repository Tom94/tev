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

#include <nanogui/opengl.h>

#include <utf8.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#ifdef _WIN32
#    include <Shlobj.h>
#else
#    include <cstring>
#    include <fcntl.h>
#    include <pwd.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif
#if !defined(__APPLE__) and !defined(_WIN32)
#    include <sys/xattr.h>
#endif

using namespace nanogui;
using namespace std;

namespace tev {

static u8string toU8string(string_view str) {
    u8string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

static string fromU8string(u8string_view str) {
    string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

string ensureUtf8(string_view str) {
    string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

string utf16to8(wstring_view utf16) {
    string utf8;
    utf8::utf16to8(begin(utf16), end(utf16), back_inserter(utf8));
    return utf8;
}

fs::path toPath(string_view utf8) {
    // tev's strings are always utf8 encoded, however fs::path does not know this. Therefore: convert the string to a u8string and pass
    // _that_ string to the fs::path constructor, which will then take care of converting the utf8 string to the native file name encoding.
    return toU8string(utf8);
}

string toString(const fs::path& path) {
    // Conversely to `toPath`, ensure that the returned string is utf8 encoded by requesting a u8string from the path object and then
    // convert _that_ string to a regular char string.
    return fromU8string(path.u8string());
}

string toDisplayString(const fs::path& path) {
#if !defined(__APPLE__) and !defined(_WIN32)
    // When running under Flatpak, we don't necessarily get real paths from the
    // host system but rather files that were passed to us using the [document portal][1]
    // (`/run/user/$UID/doc/$DOC_ID/filename`). These are not nice to show to the user.
    // Luckily, the "real" (host) path is set as an extended attribute.
    // [1]: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Documents.html#org-freedesktop-portal-documents-gethostpaths
    const char* HOST_PATH_XATTR = "user.document-portal.host-path";
    ssize_t size_hint = getxattr(path.c_str(), HOST_PATH_XATTR, nullptr, 0);
    if (size_hint < 0) {
        return toString(path);
    }

    string buffer;
    buffer.resize(size_hint);
    ssize_t actual_size = getxattr(path.c_str(), HOST_PATH_XATTR, buffer.data(), size_hint);
    if (actual_size < 0) {
        return toString(path);
    }

    buffer.resize(actual_size);
    return ensureUtf8(buffer);
#else
    return toString(path);
#endif
}

bool naturalCompare(string_view a, string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit(a[i]) && isdigit(b[j])) {
            size_t aNum = 0;
            size_t bNum = 0;

            do {
                aNum = aNum * 10 + (a[i++] - '0');
            } while (i < a.size() && isdigit(a[i]));

            do {
                bNum = bNum * 10 + (b[j++] - '0');
            } while (j < b.size() && isdigit(b[j]));

            if (aNum != bNum) {
                return aNum < bNum;
            }
        } else {
            char lowerA = tolower(a[i++]), lowerB = tolower(b[j++]);
            if (lowerA != lowerB) {
                return lowerA < lowerB;
            }
        }
    }

    return a.size() - i < b.size() - j;
}

static constexpr string_view ws = " \t\n\r\f\v";

vector<string_view> split(string_view text, string_view delim, bool inclusive) {
    vector<string_view> result;
    size_t begin = 0;
    while (true) {
        size_t end = text.find_first_of(delim, begin);
        if (end == string::npos) {
            result.emplace_back(text.substr(begin));
            break;
        } else {
            result.emplace_back(text.substr(begin, end + (inclusive ? 1 : 0) - begin));
            begin = end + 1;
        }
    }

    TEV_ASSERT(!result.empty(), "Result of split must not be empty.");
    return result;
}

vector<string_view> splitWhitespace(string_view s, bool inclusive) { return split(s, ws, inclusive); }

string toLower(string_view str) {
    string result{str};
    transform(begin(result), end(result), begin(result), [](unsigned char c) { return (char)tolower(c); });
    return result;
}

string toUpper(string_view str) {
    string result{str};
    transform(begin(result), end(result), begin(result), [](unsigned char c) { return (char)toupper(c); });
    return result;
}

string_view trimLeft(string_view s) {
    s.remove_prefix(std::min(s.find_first_not_of(ws), s.size()));
    return s;
}

string_view trimRight(string_view s) {
    if (!s.empty()) {
        s.remove_suffix(s.size() - s.find_last_not_of(ws) - 1);
    }

    return s;
}

string_view trim(string_view s) { return trimRight(trimLeft(s)); }

string substituteCurly(string_view str, const function<string(string_view)>& replacer) {
    ostringstream result;
    size_t pos = 0;
    while (true) {
        const size_t openBrace = str.find('{', pos);
        if (openBrace == string::npos) {
            result << str.substr(pos);
            break;
        }

        result << str.substr(pos, openBrace - pos);
        const size_t closeBrace = str.find('}', openBrace + 1);
        if (closeBrace == string::npos) {
            // No matching closing brace, append the rest of the string and break.
            result << str.substr(openBrace);
            break;
        }

        const string_view key = str.substr(openBrace + 1, closeBrace - openBrace - 1);
        result << replacer(key);

        pos = closeBrace + 1;
    }

    return result.str();
}

Color parseColor(string_view str) {
    if (str.empty()) {
        return Color{0, 0, 0, 0};
    }

    if (str.starts_with("#")) {
        unsigned int hexValue = 0;
        istringstream iss(string{str.substr(1)});
        iss >> hex >> hexValue;

        if (str.size() == 4) {
            // #RGB
            return Color{
                toLinear(((hexValue >> 8) & 0xF) / 15.0f),
                toLinear(((hexValue >> 4) & 0xF) / 15.0f),
                toLinear((hexValue & 0xF) / 15.0f),
                1.0f,
            };
        } else if (str.size() == 5) {
            // #RGBA
            return Color{
                toLinear(((hexValue >> 12) & 0xF) / 15.0f),
                toLinear(((hexValue >> 8) & 0xF) / 15.0f),
                toLinear(((hexValue >> 4) & 0xF) / 15.0f),
                (hexValue & 0xF) / 15.0f,
            };
        } else if (str.size() == 7) {
            // #RRGGBB
            return Color{
                toLinear(((hexValue >> 16) & 0xFF) / 255.0f),
                toLinear(((hexValue >> 8) & 0xFF) / 255.0f),
                toLinear((hexValue & 0xFF) / 255.0f),
                1.0f,
            };
        } else if (str.size() == 9) {
            // #RRGGBBAA
            return Color{
                toLinear(((hexValue >> 24) & 0xFF) / 255.0f),
                toLinear(((hexValue >> 16) & 0xFF) / 255.0f),
                toLinear(((hexValue >> 8) & 0xFF) / 255.0f),
                (hexValue & 0xFF) / 255.0f,
            };
        } else {
            throw runtime_error{fmt::format("Invalid hex color format: {}", str)};
        }
    }

    const auto parts = split(str, ",");
    if (parts.size() < 3 || parts.size() > 4) {
        throw runtime_error{fmt::format("Invalid color format: {}", str)};
    }

    Color color = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < parts.size(); ++i) {
        try {
            color[i] = stof(string{trim(parts[i])});
        } catch (const invalid_argument&) {
            throw runtime_error{fmt::format("Invalid color component: {}", parts[i])};
        } catch (const out_of_range&) { throw runtime_error{fmt::format("Color component out of range: {}", parts[i])}; }
    }

    return color;
}

bool matchesFuzzy(string_view text, string_view filter, size_t* matchedPartId) {
    if (matchedPartId) {
        // Default value of 0. Is returned when the filter is empty, when there is no match, or when the filter is a regex.
        *matchedPartId = 0;
    }

    if (filter.empty()) {
        return true;
    }

    if (filter.front() == '^' || filter.back() == '$') {
        // If the filter starts with ^ or ends in a $, we'll use regex matching. Otherwise, we use simple substring matching. Regex matching
        // is always case sensitive.
        return matchesRegex(text, filter);
    }

    // Perform matching via smart casing: if the filter is all lowercase, we want to match case-insensitively. If the filter contains any
    // uppercase characters, we want to match case-sensitively.
    const bool caseInsensitive = all_of(begin(filter), end(filter), [](char c) { return islower(c); });

    string casedText, casedFilter;
    if (caseInsensitive) {
        casedText = toLower(text);
        casedFilter = toLower(filter);

        // Update views to point to local cased strings
        text = casedText;
        filter = casedFilter;
    }

    auto words = split(filter, ", ");
    // We don't want people entering multiple spaces in a row to match everything.
    words.erase(remove(begin(words), end(words), ""), end(words));

    if (words.empty()) {
        return true;
    }

    // Match every word of the filter separately.
    for (size_t i = 0; i < words.size(); ++i) {
        if (text.find(words[i]) != string::npos) {
            if (matchedPartId) {
                *matchedPartId = i;
            }

            return true;
        }
    }

    return false;
}

bool matchesRegex(string_view text, string_view filter) {
    if (filter.empty()) {
        return true;
    }

    try {
        // regex doesn't support string_view yet
        regex searchRegex{string{filter}, regex_constants::ECMAScript | regex_constants::icase};
        return regex_search(string{text}, searchRegex);
    } catch (const regex_error&) { return false; }
}

void drawTextWithShadow(NVGcontext* ctx, float x, float y, string_view text, float shadowAlpha) {
    nvgSave(ctx);
    nvgFontBlur(ctx, 2);
    nvgFillColor(ctx, Color{0.0f, shadowAlpha});
    nvgText(ctx, x + 1, y + 1, text.data(), text.data() + text.size());
    nvgRestore(ctx);
    nvgText(ctx, x, y, text.data(), text.data() + text.size());
}

int maxTextureSize() {
#if NANOGUI_USE_METAL
    // There's unfortunately not a nice API to query this in Metal. Thankfully, macs all have had the same limit so far.
    return 16384;
#else
    static struct MaxSize {
        MaxSize() { glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value); }
        GLint value;
    } maxSize;
    return maxSize.value;
#endif
}

size_t nextSupportedTextureChannelCount(size_t channelCount) {
    if (channelCount == 0 || channelCount > 4) {
        throw runtime_error{fmt::format("Unsupported number of texture channels: {}", channelCount)};
    }

#if NANOGUI_USE_METAL
    // Metal only supports 1, 2, and 4 channel textures.
    if (channelCount == 3) {
        return 4;
    }
#endif

    return channelCount;
}

int lastError() {
#ifdef _WIN32
    return GetLastError();
#else
    return errno;
#endif
}

int lastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

string errorString(int errorId) {
#ifdef _WIN32
    char* s = NULL;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorId,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&s,
        0,
        NULL
    );

    string result = fmt::format("{} ({})", s, errorId);
    LocalFree(s);

    return result;
#else
    return fmt::format("{} ({})", strerror(errorId), errno);
#endif
}

fs::path homeDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, path) != S_OK) {
        return "";
    }

    return utf16to8(path);
#else
    struct passwd* pw = getpwuid(getuid());
    return pw->pw_dir;
#endif
}

fs::path runtimeDirectory() {
#ifdef _WIN32
    return homeDirectory() / "AppData" / "Local" / "tev";
#else
    const char* xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");
    if (!xdgRuntimeDir || !*xdgRuntimeDir) {
        return fs::path{"/tmp"};
    }

    const auto fpi = flatpakInfo();
    if (!fpi) {
        return xdgRuntimeDir;
    }

    return fs::path{xdgRuntimeDir} / "app" / fpi->flatpakId;
#endif
}

void toggleConsole() {
#ifdef _WIN32
    HWND console = GetConsoleWindow();
    DWORD consoleProcessId;
    GetWindowThreadProcessId(console, &consoleProcessId);

    // Only toggle the console if it was actually spawned by tev. If we are
    // running in a foreign console, then we should leave it be.
    if (GetCurrentProcessId() == consoleProcessId) {
        ShowWindow(console, IsWindowVisible(console) ? SW_HIDE : SW_SHOW);
    }
#endif
}

static atomic<bool> sShuttingDown{false};

bool shuttingDown() { return sShuttingDown; }

void setShuttingDown() { sShuttingDown = true; }

const optional<FlatpakInfo>& flatpakInfo() {
    const auto getFlatpakInfo = []() -> optional<FlatpakInfo> {
        const char* flatpakId = getenv("FLATPAK_ID");
        if (!flatpakId || !*flatpakId) {
            return nullopt;
        }

        FlatpakInfo info;
        info.flatpakId = flatpakId;

        ifstream idFile{"/.flatpak-info"};
        if (!idFile) {
            return info;
        }

        string line;
        string currentSection = "";
        while (getline(idFile, line)) {
            if (line.empty()) {
                continue;
            }

            if (line.front() == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.size() - 2);
                continue;
            }

            const auto parts = split(line, "=");
            if (parts.size() < 2) {
                continue;
            }

            info.metadata[currentSection][parts[0]] = line.substr(parts[0].size() + 1);
        }

        return info;
    };

    static optional<FlatpakInfo> sFlatpakInfo = getFlatpakInfo();
    return sFlatpakInfo;
}

string_view toString(EAlphaKind kind) {
    switch (kind) {
        case EAlphaKind::PremultipliedNonlinear: return "premultiplied_nonlinear";
        case EAlphaKind::Premultiplied: return "premultiplied";
        case EAlphaKind::Straight: return "straight";
        case EAlphaKind::None: return "none";
        default: throw runtime_error{"Unknown alpha kind."};
    }
}

EInterpolationMode toInterpolationMode(string_view name) {
    // Perform matching on uppercase strings
    const auto upperName = toUpper(name);
    if (upperName == "NEAREST") {
        return Nearest;
    } else if (upperName == "BILINEAR") {
        return Bilinear;
    } else if (upperName == "TRILINEAR" || upperName == "FC") {
        return Trilinear;
    } else {
        return Nearest;
    }
}

string_view toString(EInterpolationMode mode) {
    switch (mode) {
        case Nearest: return "NEAREST";
        case Bilinear: return "BILINEAR";
        case Trilinear: return "TRILINEAR";
        default: throw runtime_error{"Unknown interpolation mode."};
    }
}

ETonemap toTonemap(string_view name) {
    // Perform matching on uppercase strings
    const auto upperName = toUpper(name);
    if (upperName == "NONE") {
        return ETonemap::None;
    } else if (upperName == "SRGB") {
        return ETonemap::SRGB;
    } else if (upperName == "GAMMA") {
        return ETonemap::Gamma;
    } else if (upperName == "FALSECOLOR" || upperName == "FC") {
        return ETonemap::FalseColor;
    } else if (upperName == "POSITIVENEGATIVE" || upperName == "POSNEG" || upperName == "PN" || upperName == "+-") {
        return ETonemap::PositiveNegative;
    } else {
        return ETonemap::None;
    }
}

EMetric toMetric(string_view name) {
    // Perform matching on uppercase strings
    const auto upperName = toUpper(name);
    if (upperName == "E") {
        return EMetric::Error;
    } else if (upperName == "AE") {
        return EMetric::AbsoluteError;
    } else if (upperName == "SE") {
        return EMetric::SquaredError;
    } else if (upperName == "RAE") {
        return EMetric::RelativeAbsoluteError;
    } else if (upperName == "RSE") {
        return EMetric::RelativeSquaredError;
    } else {
        return EMetric::Error;
    }
}

string_view toString(EOrientation orientation) {
    switch (orientation) {
        case None: return "none";
        case TopLeft: return "topleft";
        case TopRight: return "topright";
        case BottomRight: return "bottomright";
        case BottomLeft: return "bottomleft";
        case LeftTop: return "lefttop";
        case RightTop: return "righttop";
        case RightBottom: return "rightbottom";
        case LeftBottom: return "leftbottom";
        default: throw runtime_error{"Unknown orientation."};
    }
}

string_view toString(EPixelType kind) {
    switch (kind) {
        case EPixelType::Uint: return "uint";
        case EPixelType::Int: return "int";
        case EPixelType::Float: return "float";
        default: throw runtime_error{"Unknown pixel type."};
    }
}

string_view toString(EPixelFormat format) {
    switch (format) {
        case EPixelFormat::U8: return "uint8";
        case EPixelFormat::U16: return "uint16";
        case EPixelFormat::U32: return "uint32";
        case EPixelFormat::I8: return "int8";
        case EPixelFormat::I16: return "int16";
        case EPixelFormat::I32: return "int32";
        case EPixelFormat::F16: return "float16";
        case EPixelFormat::F32: return "float32";
        default: throw runtime_error{"Unknown pixel format."};
    }
}

EPixelType pixelType(EPixelFormat format) {
    switch (format) {
        case EPixelFormat::U8:
        case EPixelFormat::U16:
        case EPixelFormat::U32: return EPixelType::Uint;
        case EPixelFormat::I8:
        case EPixelFormat::I16:
        case EPixelFormat::I32: return EPixelType::Int;
        case EPixelFormat::F16:
        case EPixelFormat::F32: return EPixelType::Float;
        default: throw runtime_error{"Unknown pixel format."};
    }
}

} // namespace tev
