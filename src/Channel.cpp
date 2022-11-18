// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/Channel.h>
#include <tev/ThreadPool.h>

#include <numeric>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

pair<string, string> Channel::split(const string& channel) {
    size_t dotPosition = channel.rfind(".");
    if (dotPosition != string::npos) {
        return {channel.substr(0, dotPosition + 1), channel.substr(dotPosition + 1)};
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

bool Channel::isAlpha(const string& channel) {
    return toLower(tail(channel)) == "a";
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

Channel::Channel(const std::string& name, const nanogui::Vector2i& size)
: mName{name}, mSize{size} {
    mData.resize((size_t)mSize.x() * mSize.y());
}

Task<void> Channel::divideByAsync(const Channel& other, int priority) {
    co_await ThreadPool::global().parallelForAsync<size_t>(0, other.numPixels(), [&](size_t i) {
        if (other.at(i) != 0) {
            at(i) /= other.at(i);
        } else {
            at(i) = 0;
        }
    }, priority);
}

Task<void> Channel::multiplyWithAsync(const Channel& other, int priority) {
    co_await ThreadPool::global().parallelForAsync<size_t>(0, other.numPixels(), [&](size_t i) {
        at(i) *= other.at(i);
    }, priority);
}

void Channel::updateTile(int x, int y, int width, int height, const vector<float>& newData) {
    if (x < 0 || y < 0 || x + width > size().x() || y + height > size().y()) {
        tlog::warning() << "Tile [" << x << "," << y << "," << width << "," << height << "] could not be updated because it does not fit into the channel's size " << size();
        return;
    }

    for (int posY = 0; posY < height; ++posY) {
        for (int posX = 0; posX < width; ++posX) {
            at({x + posX, y + posY}) = newData[posX + posY * width];
        }
    }
}

TEV_NAMESPACE_END
