/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tev {

template <typename T, typename Cmp = std::less<T>> class PriorityQueue {
    std::vector<T> mData;
    Cmp mCmp;

public:
    explicit PriorityQueue(Cmp cmp = Cmp{}) : mCmp{cmp} {}

    template <typename Iter> PriorityQueue(Iter first, Iter last, Cmp cmp = Cmp{}) : mData{first, last}, mCmp{cmp} {
        std::make_heap(mData.begin(), mData.end(), mCmp);
    }

    void push(const T& val) {
        mData.push_back(val);
        std::push_heap(mData.begin(), mData.end(), mCmp);
    }

    void push(T&& val) {
        mData.push_back(std::move(val));
        std::push_heap(mData.begin(), mData.end(), mCmp);
    }

    T pop() {
        if (mData.empty()) {
            throw std::runtime_error{"pop from empty queue"};
        }

        std::pop_heap(mData.begin(), mData.end(), mCmp);
        T val = std::move(mData.back());
        mData.pop_back();
        return val;
    }

    const T& top() const & { return mData.front(); }
    T& top() & { return mData.front(); }

    bool empty() const { return mData.empty(); }
    std::size_t size() const { return mData.size(); }
};

} // namespace tev
