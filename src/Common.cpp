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

#include <tev/Common.h>

#include <nanogui/opengl.h>

#include <utf8.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <regex>

#ifdef _WIN32
#   include <Shlobj.h>
#else
#   include <cstring>
#   include <pwd.h>
#   include <unistd.h>
#endif

using namespace nanogui;
using namespace std;

namespace tev {

static u8string toU8string(const string& str) {
    u8string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

static string fromU8string(const u8string& str) {
    string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

string ensureUtf8(const string& str) {
    string temp;
    utf8::replace_invalid(begin(str), end(str), back_inserter(temp));
    return temp;
}

string utf16to8(const wstring& utf16) {
    string utf8;
    utf8::utf16to8(begin(utf16), end(utf16), back_inserter(utf8));
    return utf8;
}

fs::path toPath(const string& utf8) {
    // tev's strings are always utf8 encoded, however fs::path does not know this. Therefore: convert the string to a std::u8string and pass
    // _that_ string to the fs::path constructor, which will then take care of converting the utf8 string to the native file name encoding.
    return toU8string(utf8);
}

string toString(const fs::path& path) {
    // Conversely to `toPath`, ensure that the returned string is utf8 encoded by requesting a std::u8string from the path object and then
    // convert _that_ string to a regular char string.
    return fromU8string(path.u8string());
}

bool naturalCompare(const string& a, const string& b) {
    // Compare two strings in a natural way. This means that "10" comes after "2" and before "20". This is done by splitting the strings
    // into substrings of digits and non-digits and comparing them separately.
    auto splitDigits = [](const string& str) {
        const string digits = "0123456789";
        vector<string> result;

        size_t begin = 0;
        while (true) {
            size_t end = str.find_first_of(digits, begin);
            if (end == string::npos) {
                result.emplace_back(str.substr(begin));
                break;
            } else {
                result.emplace_back(str.substr(begin, end - begin));

                begin = end;
                end = str.find_first_not_of(digits, begin);
                if (end == string::npos) {
                    result.emplace_back(str.substr(begin));
                    break;
                }

                result.emplace_back(str.substr(begin, end - begin));
                begin = end;
            }
        }

        return result;
    };

    auto aParts = splitDigits(a);
    auto bParts = splitDigits(b);

    for (size_t i = 0; i < min(aParts.size(), bParts.size()); ++i) {
        if (aParts[i] != bParts[i]) {
            // Compare the substrings. If they are both digits, compare them as numbers. Otherwise, compare them case-insensitively as strings.
            if (isdigit(aParts[i][0]) && isdigit(bParts[i][0])) {
                return stoi(aParts[i]) < stoi(bParts[i]);
            } else {
                return toLower(aParts[i]) < toLower(bParts[i]);
            }
        }
    }

    return aParts.size() < bParts.size();
}

vector<string> split(string text, const string& delim) {
    vector<string> result;
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

string toLower(string str) {
    transform(begin(str), end(str), begin(str), [](unsigned char c) { return (char)tolower(c); });
    return str;
}

string toUpper(string str) {
    transform(begin(str), end(str), begin(str), [](unsigned char c) { return (char)toupper(c); });
    return str;
}

bool matchesFuzzy(string text, string filter, size_t* matchedPartId) {
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
    if (caseInsensitive) {
        text = toLower(text);
        filter = toLower(filter);
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

bool matchesRegex(string text, string filter) {
    if (filter.empty()) {
        return true;
    }

    try {
        regex searchRegex{filter, std::regex_constants::ECMAScript | std::regex_constants::icase};
        return regex_search(text, searchRegex);
    } catch (const regex_error&) { return false; }
}

void drawTextWithShadow(NVGcontext* ctx, float x, float y, string text, float shadowAlpha) {
    nvgSave(ctx);
    nvgFontBlur(ctx, 2);
    nvgFillColor(ctx, Color{0.0f, shadowAlpha});
    nvgText(ctx, x + 1, y + 1, text.c_str(), NULL);
    nvgRestore(ctx);
    nvgText(ctx, x, y, text.c_str(), NULL);
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

EInterpolationMode toInterpolationMode(string name) {
    // Perform matching on uppercase strings
    name = toUpper(name);
    if (name == "NEAREST") {
        return Nearest;
    } else if (name == "BILINEAR") {
        return Bilinear;
    } else if (name == "TRILINEAR" || name == "FC") {
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

ETonemap toTonemap(string name) {
    // Perform matching on uppercase strings
    name = toUpper(name);
    if (name == "SRGB") {
        return SRGB;
    } else if (name == "GAMMA") {
        return Gamma;
    } else if (name == "FALSECOLOR" || name == "FC") {
        return FalseColor;
    } else if (name == "POSITIVENEGATIVE" || name == "POSNEG" || name == "PN" || name == "+-") {
        return PositiveNegative;
    } else {
        return SRGB;
    }
}

EMetric toMetric(string name) {
    // Perform matching on uppercase strings
    name = toUpper(name);
    if (name == "E") {
        return Error;
    } else if (name == "AE") {
        return AbsoluteError;
    } else if (name == "SE") {
        return SquaredError;
    } else if (name == "RAE") {
        return RelativeAbsoluteError;
    } else if (name == "RSE") {
        return RelativeSquaredError;
    } else {
        return Error;
    }
}

} // namespace tev
