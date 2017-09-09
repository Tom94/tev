// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Common.h"

#include <algorithm>
#include <cctype>
#include <map>

#ifdef _WIN32
#include <Windows.h>
#endif

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

bool matches(string text, string filter) {
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

void toggleConsole() {
#ifdef _WIN32
    auto console = GetConsoleWindow();
    ShowWindow(console, IsWindowVisible(console) ? SW_HIDE : SW_SHOW);
#endif
}

TEV_NAMESPACE_END
