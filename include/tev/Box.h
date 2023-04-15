// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>


namespace tev {

template <typename T, uint32_t N_DIMS>
struct Box {
    using Vector = nanogui::Array<T, N_DIMS>;

    Box(const Vector& min, const Vector& max) : min{min}, max{max} {}
    Box(const Vector& max) : Box{Vector{(T)0}, max} {}
    Box() : Box{Vector{std::numeric_limits<T>::max()}, Vector{std::numeric_limits<T>::min()}} {}

    // Casting boxes of other types to this one
    template <typename U>
    Box(const Box<U, N_DIMS>& other) : min{other.min}, max{other.max} {}

    Vector size() const {
        return max - min;
    }

    Vector middle() const {
        return (min + max) / (T)2;
    }

    bool isValid() const {
        bool result = true;
        for (uint32_t i = 0; i < N_DIMS; ++i) {
            result &= max[i] >= min[i];
        }
        return result;
    }

    bool contains(const Vector& pos) const {
        bool result = true;
        for (uint32_t i = 0; i < N_DIMS; ++i) {
            result &= pos[i] >= min[i] && pos[i] < max[i];
        }
        return result;
    }

    bool operator==(const Box& other) const {
        return min == other.min && max == other.max;
    }

    Box<T, N_DIMS> inflate(T amount) const {
        return {min - Vector{amount}, max + Vector{amount}};
    }

    Vector min, max;
};

using Box2f = Box<float, 2>;
using Box3f = Box<float, 3>;
using Box4f = Box<float, 4>;
using Box2i = Box<int32_t, 2>;
using Box3i = Box<int32_t, 3>;
using Box4i = Box<int32_t, 4>;

}
