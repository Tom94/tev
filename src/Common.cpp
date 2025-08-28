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
#include <iostream>
#include <map>
#include <regex>

#ifdef _WIN32
#    include <Shlobj.h>
#else
#    include <cstring>
#    include <fcntl.h>
#    include <pwd.h>
#    include <sys/wait.h>
#    include <unistd.h>
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
    // tev's strings are always utf8 encoded, however fs::path does not know this. Therefore: convert the string to a std::u8string and pass
    // _that_ string to the fs::path constructor, which will then take care of converting the utf8 string to the native file name encoding.
    return toU8string(utf8);
}

string toString(const fs::path& path) {
    // Conversely to `toPath`, ensure that the returned string is utf8 encoded by requesting a std::u8string from the path object and then
    // convert _that_ string to a regular char string.
    return fromU8string(path.u8string());
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

vector<string_view> split(string_view text, string_view delim) {
    vector<string_view> result;
    size_t begin = 0;
    while (true) {
        size_t end = text.find_first_of(delim, begin);
        if (end == string::npos) {
            result.emplace_back(text.substr(begin));
            break;
        } else {
            result.emplace_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
    }

    return result;
}

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
        // std::regex doesn't support std::string_view yet
        regex searchRegex{string{filter}, std::regex_constants::ECMAScript | std::regex_constants::icase};
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
    char path[MAX_PATH];
    if (SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, 0, path) != S_OK) {
        return "";
    }

    return path;
#else
    struct passwd* pw = getpwuid(getuid());
    return pw->pw_dir;
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

static std::atomic<bool> sShuttingDown{false};

bool shuttingDown() { return sShuttingDown; }

void setShuttingDown() { sShuttingDown = true; }

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

string toString(EInterpolationMode mode) {
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
    if (upperName == "SRGB") {
        return SRGB;
    } else if (upperName == "GAMMA") {
        return Gamma;
    } else if (upperName == "FALSECOLOR" || upperName == "FC") {
        return FalseColor;
    } else if (upperName == "POSITIVENEGATIVE" || upperName == "POSNEG" || upperName == "PN" || upperName == "+-") {
        return PositiveNegative;
    } else {
        return SRGB;
    }
}

EMetric toMetric(string_view name) {
    // Perform matching on uppercase strings
    const auto upperName = toUpper(name);
    if (upperName == "E") {
        return Error;
    } else if (upperName == "AE") {
        return AbsoluteError;
    } else if (upperName == "SE") {
        return SquaredError;
    } else if (upperName == "RAE") {
        return RelativeAbsoluteError;
    } else if (upperName == "RSE") {
        return RelativeSquaredError;
    } else {
        return Error;
    }
}

string toString(EPixelFormat format) {
    switch (format) {
        case EPixelFormat::U8: return "U8";
        case EPixelFormat::U16: return "U16";
        case EPixelFormat::F16: return "F16";
        case EPixelFormat::F32: return "F32";
        default: throw runtime_error{"Unknown pixel format."};
    }
}

} // namespace tev
