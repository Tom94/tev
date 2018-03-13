// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Common.h>

#include <nanogui/opengl.h>

#include <algorithm>
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

TEV_NAMESPACE_BEGIN

vector<string> split(string text, const string& delim) {
    vector<string> result;
    while (true) {
        size_t begin = text.find_last_of(delim);
        if (begin == string::npos) {
            result.emplace_back(text);
            return result;
        } else {
            result.emplace_back(text.substr(begin + 1));
            text.resize(begin);
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

bool endsWith(const string& str, const string& ending) {
    return str.rfind(ending) == str.length() - ending.length();
}

bool matches(string text, string filter, bool isRegex) {
    auto matchesFuzzy = [](string text, string filter) {
        if (filter.empty()) {
            return true;
        }

        // Perform matching on lowercase strings
        text = toLower(text);
        filter = toLower(filter);

        auto words = split(filter, ", ");
        // We don't want people entering multiple spaces in a row to match everything.
        words.erase(remove(begin(words), end(words), ""), end(words));

        if (words.empty()) {
            return true;
        }

        // Match every word of the filter separately.
        for (const auto& word : words) {
            if (text.find(word) != string::npos) {
                return true;
            }
        }

        return false;
    };

    auto matchesRegex = [](string text, string filter) {
        if (filter.empty()) {
            return true;
        }

        try {
            regex searchRegex{filter, std::regex_constants::ECMAScript | std::regex_constants::icase};
            return regex_search(text, searchRegex);
        } catch (const regex_error&) {
            return false;
        }
    };

    return isRegex ? matchesRegex(text, filter) : matchesFuzzy(text, filter);
}

void drawTextWithShadow(NVGcontext* ctx, float x, float y, string text, float shadowAlpha) {
    nvgSave(ctx);
    nvgFontBlur(ctx, 2);
    nvgFillColor(ctx, Color{0.0f, shadowAlpha});
    nvgText(ctx, x + 1, y + 1, text.c_str(), NULL);
    nvgRestore(ctx);
    nvgText(ctx, x, y, text.c_str(), NULL);
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
    } else if (name == "POSITIVENEGATIVE" || name == "POSNEG" || name == "PN" ||name == "+-") {
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
        NULL, errorId, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&s, 0, NULL);

    string result = tfm::format("%s (%d)", s, errorId);
    LocalFree(s);

    return result;
#else
    return tfm::format("%s (%d)", strerror(errorId), errno);
#endif
}

string absolutePath(const string& path) {
    char buffer[16384];

#ifdef _WIN32
    DWORD length = GetFullPathName(path.c_str(), sizeof(buffer), buffer, NULL);
    if (length == 0 || length == sizeof(buffer)) {
        throw runtime_error{tfm::format("Could not obtain absolute path: %s", errorString(lastError()))};
    }
    return buffer;
#else
    if (realpath(path.c_str(), buffer) == NULL) {
        throw runtime_error{tfm::format("Could not obtain absolute path: %s", errorString(lastError()))};
    }
    return buffer;
#endif
}

string homeDirectory() {
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

TEV_NAMESPACE_END
