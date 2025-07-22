/*
 * tev -- the EDR viewer
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

#include <tev/Channel.h>
#include <tev/ThreadPool.h>

#include <memory>

using namespace nanogui;
using namespace std;

namespace tev {

pair<string_view, string_view> Channel::split(string_view channel) {
    size_t dotPosition = channel.rfind(".");
    if (dotPosition != string::npos) {
        return {channel.substr(0, dotPosition + 1), channel.substr(dotPosition + 1)};
    }

    return {"", channel};
}

string_view Channel::tail(string_view channel) { return split(channel).second; }

string_view Channel::head(string_view channel) { return split(channel).first; }

bool Channel::isTopmost(string_view channel) { return tail(channel) == channel; }

bool Channel::isAlpha(string_view channel) { return toLower(tail(channel)) == "a"; }

Color Channel::color(string_view channel) {
    auto lowerChannel = toLower(tail(channel));

    if (channel == "r") {
        return Color(0.8f, 0.2f, 0.2f, 1.0f);
    } else if (channel == "g") {
        return Color(0.2f, 0.8f, 0.2f, 1.0f);
    } else if (channel == "b") {
        return Color(0.2f, 0.3f, 1.0f, 1.0f);
    }

    return Color(1.0f, 1.0f);
}

Channel::Channel(string_view name, const nanogui::Vector2i& size, shared_ptr<vector<float>> data, size_t dataOffset, size_t dataStride) :
    mName{name}, mSize{size} {
    if (data) {
        mData = data;
        mDataOffset = dataOffset;
        mDataStride = dataStride;
    } else {
        mData = make_shared<vector<float>>((size_t)size.x() * size.y());
        mDataOffset = 0;
        mDataStride = 1;
    }
}

Task<void> Channel::divideByAsync(const Channel& other, int priority) {
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        other.numPixels(),
        [&](size_t i) {
            if (other.at(i) != 0) {
                at(i) /= other.at(i);
            } else {
                at(i) = 0;
            }
        },
        priority
    );
}

Task<void> Channel::multiplyWithAsync(const Channel& other, int priority) {
    co_await ThreadPool::global().parallelForAsync<size_t>(0, other.numPixels(), [&](size_t i) { at(i) *= other.at(i); }, priority);
}

void Channel::updateTile(int x, int y, int width, int height, span<const float> newData) {
    if (x < 0 || y < 0 || x + width > size().x() || y + height > size().y()) {
        tlog::warning() << "Tile [" << x << "," << y << "," << width << "," << height
                        << "] could not be updated because it does not fit into the channel's size " << size();
        return;
    }

    for (int posY = 0; posY < height; ++posY) {
        for (int posX = 0; posX < width; ++posX) {
            at({x + posX, y + posY}) = newData[posX + posY * (size_t)width];
        }
    }
}

} // namespace tev
