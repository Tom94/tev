// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Channel.h"

using namespace Eigen;
using namespace std;

TEV_NAMESPACE_BEGIN

Channel::Channel(const std::string& name, Vector2i size)
: mName{name}, mSize{size} {
}

string Channel::tail(const string& channel) {
    size_t dotPosition = channel.rfind(".");
    if (dotPosition != string::npos) {
        return channel.substr(dotPosition + 1);
    }

    return channel;
}

TEV_NAMESPACE_END
