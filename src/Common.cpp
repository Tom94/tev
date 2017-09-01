// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Common.h"

#include <algorithm>
#include <map>

using namespace std;

TEV_NAMESPACE_BEGIN

bool matches(string text, string filter) {
    if (filter.empty()) {
        return true;
    }

    // Perform matching on lowercase strings
    transform(begin(text), end(text), begin(text), ::tolower);
    transform(begin(filter), end(filter), begin(filter), ::tolower);

    return text.find(filter) != string::npos;
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
