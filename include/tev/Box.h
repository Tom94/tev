// This file was developed by Thomas Müller <contact@tom94.net>.
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

    Box(const std::vector<Vector>& points) : Box() {
        for (const auto& point : points) {
            min = nanogui::min(min, point);
            max = nanogui::max(max, point);
        }
    }

    Vector size() const {
        return nanogui::max(max - min, Vector{(T)0});
    }

    using area_t = std::conditional_t<std::is_integral_v<T>, size_t, T>;
    area_t area() const {
        auto size = this->size();
        area_t result = (T)1;
        for (uint32_t i = 0; i < N_DIMS; ++i) {
            result *= (area_t)size[i];
        }

        return result;
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

    bool contains_inclusive(const Vector& pos) const {
        bool result = true;
        for (uint32_t i = 0; i < N_DIMS; ++i) {
            result &= pos[i] >= min[i] && pos[i] <= max[i];
        }

        return result;
    }

    bool contains(const Box& other) const {
        return contains_inclusive(other.min) && contains_inclusive(other.max);
    }

    Box intersect(const Box& other) const {
        return {nanogui::max(min, other.min), nanogui::min(max, other.max)};
    }

    Box translate(const Vector& offset) const {
        return {min + offset, max + offset};
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

template <typename T, uint32_t N_DIMS>
struct fmt::formatter<tev::Box<T, N_DIMS>> : fmt::formatter<std::string_view> {
    template <typename FormatContext>
    auto format(const tev::Box<T, N_DIMS>& box, FormatContext& ctx) {
        return formatter<std::string_view>::format(fmt::format("[{}, {}]", box.min, box.max), ctx);
    }
};

template <typename Stream, typename T, uint32_t N_DIMS, std::enable_if_t<std::is_base_of_v<std::ostream, Stream>, int> = 0>
Stream& operator<<(Stream& os, const tev::Box<T, N_DIMS>& v) {
    os << '[' << v.min << ", " << v.max << ']';
    return os;
}

