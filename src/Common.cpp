// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Common.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace std;

TEV_NAMESPACE_BEGIN

vector<string> split(string text, const string& delim) {
    vector<string> result;
    while (true) {
        size_t begin = text.rfind(delim);
        if (begin == string::npos) {
            result.emplace_back(text);
            return result;
        } else {
            result.emplace_back(text.substr(begin + delim.length()));
            text.resize(begin);
        }
    }

    return result;
}

bool matches(string text, string filter) {
    if (filter.empty()) {
        return true;
    }

    // Perform matching on lowercase strings
    transform(begin(text), end(text), begin(text), ::tolower);
    transform(begin(filter), end(filter), begin(filter), ::tolower);

    auto words = split(filter, " ");
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
    transform(begin(name), end(name), begin(name), ::toupper);
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
    transform(begin(name), end(name), begin(name), ::toupper);
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

TEV_NAMESPACE_END
