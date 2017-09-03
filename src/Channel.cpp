// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/Channel.h"

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

Channel::Channel(const std::string& name, Vector2i size)
: mName{name}, mSize{size} {
}

pair<string, string> Channel::split(const string& channel) {
    size_t dotPosition = channel.rfind(".");
    if (dotPosition != string::npos) {
        return make_pair(channel.substr(0, dotPosition), channel.substr(dotPosition + 1));
    }

    return make_pair(""s, channel);
}

string Channel::tail(const string& channel) {
    return split(channel).second;
}

string Channel::head(const string& channel) {
    return split(channel).first;
}

bool Channel::isTopmost(const string& channel) {
    return tail(channel) == channel;
}

Color Channel::color(string channel) {
    channel = tail(channel);

    if (channel == "R" || channel == "r") {
        return Color(0.8f, 0.2f, 0.2f, 1.0f);
    } else if (channel == "G" || channel == "g") {
        return Color(0.2f, 0.8f, 0.2f, 1.0f);
    } else if (channel == "B" || channel == "b") {
        return Color(0.2f, 0.3f, 1.0f, 1.0f);
    }

    return Color(1.0f, 1.0f);
}

TEV_NAMESPACE_END
