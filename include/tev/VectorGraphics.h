// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

struct NVGcontext;

TEV_NAMESPACE_BEGIN

struct VgCommand {
    enum class EType : int32_t {
        Save = 0,
        Restore = 1,
        MoveTo = 2,
        LineTo = 3,
        FillColor = 4,
        StrokeColor = 5,
        Fill = 6,
    };

    struct Pos { float x, y; };
    struct Color { float r, g, b, a; };

    size_t size() const;
    void apply(NVGcontext* ctx) const;

    EType type;
    std::vector<char> data;
};

TEV_NAMESPACE_END
