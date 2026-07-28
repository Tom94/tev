/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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

#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/ThreadPool.h>

#include <half.h>

#include <xsimd/xsimd.hpp>

namespace tev {

// -----------------------------------------------------------------------------
// All functions are templated on the batch type B = xsimd::batch<float, A>.
//
// Vector mode:  B = xsimd::batch<float>                    (native best arch)
//               B = xsimd::batch<float, xsimd::avx2>       (explicit arch)
// Scalar mode:  B = float                                  (size == 1)
// -----------------------------------------------------------------------------
using vf = xsimd::batch<float>;

template <class B, class = void> struct int_companion {
    using type = xsimd::batch<int32_t, typename B::arch_type>;
};
template <class B> struct int_companion<B, std::enable_if_t<std::is_arithmetic_v<B>>> {
    using type = int32_t;
};
template <class B> using int_companion_t = typename int_companion<B>::type;

template <class B, class = void> struct uint_companion {
    using type = xsimd::batch<uint32_t, typename B::arch_type>;
};
template <class B> struct uint_companion<B, std::enable_if_t<std::is_arithmetic_v<B>>> {
    using type = uint32_t;
};
template <class B> using uint_companion_t = typename uint_companion<B>::type;

inline float int_to_float(std::int32_t i) noexcept { return static_cast<float>(i); }
template <class A> xsimd::batch<float, A> int_to_float(const xsimd::batch<std::int32_t, A>& i) noexcept { return xsimd::to_float(i); }

inline int float_to_int(float f) noexcept { return static_cast<int32_t>(f); }
template <class A> xsimd::batch<int32_t, A> float_to_int(const xsimd::batch<float, A>& f) noexcept { return xsimd::to_int(f); }

inline float sum(float f) noexcept { return f; }
template <class T, class A> T sum(const xsimd::batch<T, A>& f) noexcept { return xsimd::reduce_add(f); }
inline float max(float f) noexcept { return f; }
template <class T, class A> T max(const xsimd::batch<T, A>& f) noexcept { return xsimd::reduce_max(f); }
inline float min(float f) noexcept { return f; }
template <class T, class A> T min(const xsimd::batch<T, A>& f) noexcept { return xsimd::reduce_min(f); }

inline float gather(const float* ptr, int i) noexcept { return ptr[i]; }
template <class B> vf gather(const float* ptr, const B& i) noexcept { return vf::gather(ptr, i); }

// portable round-to-nearest-even: xsimd port of Giesen's float_to_half_fast3_rtne.
// results land in the low 16 bits of an equally-wide uint32 batch.
template <class B> auto float_to_half(const B& fb) noexcept -> uint_companion_t<B> {
    using i32 = uint_companion_t<B>;
    using s32 = int_companion_t<B>;
    using f32 = B;

    const i32 sign_mask = i32(0x80000000u);
    const i32 f32infty = i32(255u << 23);
    const i32 f16max = i32((127u + 16u) << 23); // >= this rounds to +inf
    const i32 min_normal = i32((127u - 14u) << 23); // smallest fp32 -> normalized fp16
    const i32 subnorm_magic = i32(((127u - 15u) + (23u - 10u) + 1u) << 23);
    const i32 normal_bias = i32(0xfffu - ((127u - 15u) << 23));
    const i32 nan_out = i32(0x7e00u);
    const i32 inf_out = i32(0x7c00u);

    i32 u = xsimd::bit_cast<i32>(fb);
    i32 sign = u & sign_mask;
    u = u ^ sign; // abs bits
    f32 absf = xsimd::bit_cast<f32>(u);

    // classification
    auto is_nan = (u > f32infty); // strictly greater -> NaN
    auto is_regular = (u < f16max); // (sub)normal, not special
    auto is_sub = (u < min_normal); // result is subnormal fp16
    i32 special = xsimd::select(is_nan, nan_out, inf_out);

    // subnormal path: add magic, then integer-subtract the bias.
    // relies on fp addition being round-to-nearest-even.
    f32 sub1 = absf + xsimd::bit_cast<f32>(subnorm_magic);
    i32 sub = xsimd::bit_cast<i32>(sub1) - subnorm_magic;

    // normal path: RTNE via odd-mantissa bias.
    i32 mantoddbit = u << (31 - 13);        // move mantissa LSB (bit 13) to sign
    // arithmetic shift right by 31 -> all-ones if odd, zero if even.
    i32 mantodd = xsimd::bit_cast<i32>(xsimd::bit_cast<s32>(mantoddbit) >> 31);
    i32 round = (u + normal_bias) - mantodd;
    i32 normal = round >> 13;

    // combine
    i32 nonspecial = xsimd::select(is_sub, sub, normal);
    i32 out = xsimd::select(is_regular, nonspecial, special);
    out = out | (sign >> 16);
    return out;
}

// portable round-half-up: xsimd port of Giesen's float_to_half_fast3.
// operates on one float batch, returns results in the low 16 bits of an equally-wide uint32 batch.
template <class B> auto float_to_half_round_up(const B& fb) noexcept -> uint_companion_t<B> {
    using i32 = uint_companion_t<B>;

    const i32 sign_mask = i32(0x80000000u);
    const i32 round_mask = i32(~0xfffu);
    const i32 f32infty = i32(255u << 23);
    const i32 f16infty = i32(31u << 23);
    const B magic = xsimd::bit_cast<B>(i32(15u << 23));
    const i32 nan_out = i32(0x7e00u);
    const i32 inf_out = i32(0x7c00u);

    i32 u = xsimd::bit_cast<i32>(fb);
    i32 sign = u & sign_mask;
    u = u ^ sign; // abs bits

    // special-case mask: exponent all-ones (inf/nan)
    auto is_special = u >= f32infty;
    auto is_nan = u > f32infty;
    i32 special = xsimd::select(is_nan, nan_out, inf_out);

    // normal / denormal path
    i32 masked = u & round_mask;
    B scaled = xsimd::bit_cast<B>(masked) * magic;
    i32 biased = xsimd::bit_cast<i32>(scaled) - round_mask;
    biased = xsimd::min(biased, f16infty);   // clamp overflow to inf
    i32 normal = biased >> 13;

    i32 out = xsimd::select(is_special, special, normal);
    out = out | (sign >> 16);
    return out;
}

template <class B> void store_halves(const B& v, half* dst) noexcept {
    if constexpr (std::is_arithmetic_v<B>) {
        *dst = std::bit_cast<half>(static_cast<uint16_t>(v));
    } else {
        alignas(B::arch_type::alignment()) uint32_t tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t j = 0; j < B::size; ++j) {
            dst[j] = std::bit_cast<half>(static_cast<uint16_t>(tmp[j]));
        }
    }
}

// log2, ~single-precision polynomial. Clamps subnormals to FLT_MIN.
template <class B> B fastLog2(const B& x_in) noexcept {
    using vi = int_companion_t<B>;

    // x = max(x, FLT_MIN) to avoid the subnormal path.
    B x = xsimd::max(x_in, B(1.1754944e-38f));

    vi i = xsimd::bitwise_cast<int32_t>(x);

    // exponent: ((i >> 23) & 0xFF) - 127
    vi e = ((i >> 23) & vi(0xFF)) - vi(127);
    B ef = int_to_float(e);

    // mantissa in [1,2): (i & 0x007FFFFF) | 0x3F800000
    vi mi = (i & vi(0x007FFFFF)) | vi(0x3F800000);
    B m = xsimd::bitwise_cast<float>(mi);

    B p = m - B(1.0f);
    B r(0.04588701f);
    r = xsimd::fma(r, p, B(-0.19442591f));
    r = xsimd::fma(r, p, B(0.41542437f));
    r = xsimd::fma(r, p, B(-0.70868282f));
    r = xsimd::fma(r, p, B(1.44182586f));
    r = r * p;
    return r + ef;
}

template <class B> B fastLog(const B& x) noexcept { return fastLog2(x) * B(0.6931471805599453f); } // ln(2)

// exp2, ~single-precision polynomial. round-to-nearest-even + bit-injection ldexp.
template <class B> B fastExp2(const B& x) noexcept {
    using vi = int_companion_t<B>;

    // round to nearest even (matches nearbyintf under default rounding).
    B n = xsimd::rint(x);
    B f = x - n;

    B r(0.00015465312f);
    r = xsimd::fma(r, f, B(0.0013395280f));
    r = xsimd::fma(r, f, B(0.0096180400f));
    r = xsimd::fma(r, f, B(0.055503407f));
    r = xsimd::fma(r, f, B(0.24022651f));
    r = xsimd::fma(r, f, B(0.69314720f));
    r = xsimd::fma(r, f, B(1.0f));

    // scale by 2^n via exponent bits: (ni + 127) << 23
    vi ni = float_to_int(n);
    vi bias = (ni + vi(127)) << 23;
    B scale = xsimd::bitwise_cast<float>(bias);
    return r * scale;
}

template <class B> B fastExp(const B& x) noexcept { return fastExp2(x * B(1.4426950408889634f)); } // 1/ln(2)

// pow2-based pow: 2^(e * log2(x)). Requires x >= 0
template <class B> B fastPow(const B& x, const B& y) noexcept {
    B r = fastExp2(y * fastLog2(x));
    r = xsimd::select(x == B(0.0f), B(0.0f), r);
    return r;
}

template <class B, typename Value, size_t Size>
nanogui::Array<B, Size> simdMatmul(const nanogui::Matrix<Value, Size>& m, const nanogui::Array<B, Size>& v) {
    nanogui::Array<B, Size> result((Value)0);
    for (size_t k = 0; k < Size; ++k) {
        for (size_t i = 0; i < Size; ++i) {
            result.v[i] = xsimd::fma(B{m.m[k][i]}, v.v[k], result.v[i]);
        }
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> abs(const nanogui::Array<float, N_DIMS>& v) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = xsimd::abs(v[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> max(const nanogui::Array<float, N_DIMS>& a, const nanogui::Array<float, N_DIMS>& b) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = xsimd::max(a[i], b[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> min(const nanogui::Array<float, N_DIMS>& a, const nanogui::Array<float, N_DIMS>& b) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = xsimd::min(a[i], b[i]);
    }

    return result;
}

template <class B, typename T> B loadChannel(const std::span<T>& view, size_t c, size_t x, size_t y, size_t xstride, size_t ystride) {
    if constexpr (std::is_arithmetic_v<B>) {
        return B(view[c + x * xstride + y * ystride]);
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[c + (x + i) * xstride + y * ystride];
        }
        return B::load_aligned(tmp);
    }
}

template <class B, typename T> B loadChannel(const std::span<T>& view, size_t c, size_t idx, size_t stride) {
    if constexpr (std::is_arithmetic_v<B>) {
        return B(view[c + idx * stride]);
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[c + (idx + i) * stride];
        }
        return B::load_aligned(tmp);
    }
}

template <class B, typename T> B loadChannel(const T& view, size_t c, size_t x, size_t y) {
    if constexpr (std::is_arithmetic_v<B>) {
        return view[c, x, y];
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[c, x + i, y];
        }
        return B::load_aligned(tmp);
    }
}

template <class B, typename T> B loadChannel(const T& view, size_t c, size_t idx) {
    if constexpr (std::is_arithmetic_v<B>) {
        return view[c, idx];
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[c, idx + i];
        }

        return B::load_aligned(tmp);
    }
}

template <class B, size_t Size, typename T, typename... Args> nanogui::Array<B, Size> loadChannels(const T& view, Args... args) {
    nanogui::Array<B, Size> result;
    for (size_t c = 0; c < Size; ++c) {
        result.v[c] = loadChannel<B>(view, c, args...);
    }

    return result;
}

template <class B, typename T> B loadChannel(const ChannelView<T>& view, size_t x, size_t y) {
    if constexpr (std::is_arithmetic_v<B>) {
        return view[x, y];
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[x + i, y];
        }
        return B::load_aligned(tmp);
    }
}

template <class B, typename T> B loadChannel(const ChannelView<T>& view, size_t idx) {
    if constexpr (std::is_arithmetic_v<B>) {
        return view[idx];
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        for (std::size_t i = 0; i < B::size; ++i) {
            tmp[i] = view[idx + i];
        }

        return B::load_aligned(tmp);
    }
}

template <class B, typename T>
void storeChannel(const std::span<T>& view, size_t c, size_t x, size_t y, size_t xstride, size_t ystride, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view[c + x * xstride + y * ystride] = v;
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view[c + (x + i) * xstride + y * ystride] = tmp[i];
        }
    }
}

template <class B, typename T> void storeChannel(const std::span<T>& view, size_t c, size_t idx, size_t stride, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view[c + idx * stride] = v;
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view[c + (idx + i) * stride] = tmp[i];
        }
    }
}

template <class B, typename T> void storeChannel(const T& view, size_t c, size_t x, size_t y, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view[c, x, y] = v;
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view[c, x + i, y] = tmp[i];
        }
    }
}

template <class B, typename T> void storeChannel(const T& view, size_t c, size_t idx, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view[c, idx] = v;
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view[c, idx + i] = tmp[i];
        }
    }
}

template <class B, size_t Size, typename T, typename... Args>
void storeChannels(const nanogui::Array<B, Size>& v, const T& view, Args... args) {
    for (size_t c = 0; c < Size; ++c) {
        storeChannel<B>(view, c, args..., v.v[c]);
    }
}

template <class B, typename T> void storeChannel(const ChannelView<T>& view, size_t x, size_t y, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view.setAt(x, y, v);
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view.setAt(x + i, y, tmp[i]);
        }
    }
}

template <class B, typename T> void storeChannel(const ChannelView<T>& view, size_t idx, const B& v) {
    if constexpr (std::is_arithmetic_v<B>) {
        view.setAt(idx, v);
    } else {
        alignas(B::arch_type::alignment()) typename B::value_type tmp[B::size];
        v.store_aligned(tmp);
        for (std::size_t i = 0; i < B::size; ++i) {
            view.setAt(idx + i, tmp[i]);
        }
    }
}

template <std::integral Int, typename F> void simdFor(Int start, Int end, F&& f) {
    static constexpr auto SIMD_SIZE = (Int)vf::size;

    auto i = start;
    if constexpr (SIMD_SIZE > 1) {
        for (; i + SIMD_SIZE <= end; i += SIMD_SIZE) {
            f.template operator()<vf>(i);
        }
    }

    for (; i < end; ++i) {
        f.template operator()<float>(i);
    }
}

template <std::integral Int, typename F>
Task<void> simdParallelFor(ThreadPool& pool, Int start, Int end, size_t approxCost, F body, int priority) {
    co_await pool.parallelFor(start, end, approxCost, [&body](Int bStart, Int bEnd) { simdFor(bStart, bEnd, body); }, priority);
}

} // namespace tev
