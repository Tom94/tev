// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Channel.h>

#include <numeric>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

Channel::Channel(const std::string& name, Vector2i size)
: mName{name} {
    mData.resize(size.y(), size.x());
}

pair<string, string> Channel::split(const string& channel) {
    size_t dotPosition = channel.rfind(".");
    if (dotPosition != string::npos) {
        return {channel.substr(0, dotPosition), channel.substr(dotPosition + 1)};
    }

    return {"", channel};
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
    channel = toLower(tail(channel));

    if (channel == "r") {
        return Color(0.8f, 0.2f, 0.2f, 1.0f);
    } else if (channel == "g") {
        return Color(0.2f, 0.8f, 0.2f, 1.0f);
    } else if (channel == "b") {
        return Color(0.2f, 0.3f, 1.0f, 1.0f);
    }

    return Color(1.0f, 1.0f);
}

void Channel::divideByAsync(const Channel& other, ThreadPool& pool) {
    pool.parallelForNoWait<DenseIndex>(0, other.count(), [&](DenseIndex i) {
        if (other.at(i) != 0) {
            at(i) /= other.at(i);
        } else {
            at(i) = 0;
        }
    });
}

void Channel::multiplyWithAsync(const Channel& other, ThreadPool& pool) {
    pool.parallelForNoWait<DenseIndex>(0, other.count(), [&](DenseIndex i) {
        at(i) *= other.at(i);
    });
}

TEV_NAMESPACE_END
