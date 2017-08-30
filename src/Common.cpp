// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Common.h"

#include <algorithm>

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

TEV_NAMESPACE_END
