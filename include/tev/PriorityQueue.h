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

// This file was adapted from the nanogui::Graph class, which was developed
// by Wenzel Jakob <wenzel.jakob@epfl.ch> and based on the NanoVG demo application
// by Mikko Mononen. Modifications were developed by Thomas Müller <contact@tom94.net>.

#pragma once

#include <algorithm>
#include <vector>

namespace tev {

template <typename T, typename Cmp = std::less<T>> class PriorityQueue {
    std::vector<T> data_;
    Cmp cmp_;

public:
    explicit PriorityQueue(Cmp cmp = Cmp{}) : cmp_(cmp) {}

    template <typename Iter> PriorityQueue(Iter first, Iter last, Cmp cmp = Cmp{}) : data_(first, last), cmp_(cmp) {
        std::make_heap(data_.begin(), data_.end(), cmp_);
    }

    void push(const T& val) {
        data_.push_back(val);
        std::push_heap(data_.begin(), data_.end(), cmp_);
    }

    void push(T&& val) {
        data_.push_back(std::move(val));
        std::push_heap(data_.begin(), data_.end(), cmp_);
    }

    T pop() {
        if (data_.empty()) {
            throw std::runtime_error("pop from empty queue");
        }

        std::pop_heap(data_.begin(), data_.end(), cmp_);
        T val = std::move(data_.back());
        data_.pop_back();
        return val;
    }

    const T& top() const & { return data_.front(); }
    T& top() & { return data_.front(); }

    bool empty() const { return data_.empty(); }
    std::size_t size() const { return data_.size(); }
};

} // namespace tev
