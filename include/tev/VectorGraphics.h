// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

TEV_NAMESPACE_BEGIN

struct VgCommand {
    enum class EType : int8_t {
        Save = 0,
        Restore = 1,
        MoveTo = 2,
        LineTo = 3,
        FillColor = 4,
        StrokeColor = 5,
        Fill = 6,
        Stroke = 7,
        BeginPath = 8,
    };

    struct Pos { float x, y; };
    struct Color { float r, g, b, a; };

    size_t size() const {
        switch (type) {
            case EType::Save: return 0;
            case EType::Restore: return 0;
            case EType::MoveTo: return sizeof(Pos);
            case EType::LineTo: return sizeof(Pos);
            case EType::FillColor: return sizeof(Color);
            case EType::StrokeColor: return sizeof(Color);
            case EType::Fill: return 0;
            case EType::Stroke: return 0;
            case EType::BeginPath: return 0;
            default: throw std::runtime_error{"Invalid VgCommand type."};
        }
    }

    static VgCommand save() { return {EType::Save, {}}; }
    static VgCommand restore() { return {EType::Restore, {}}; }

    static VgCommand moveTo(const Pos& p) {
        VgCommand result{EType::MoveTo, {}};
        result.data.resize(result.size());
        *(Pos*)result.data.data() = p;
        return result;
    }

    static VgCommand lineTo(const Pos& p) {
        VgCommand result{EType::LineTo, {}};
        result.data.resize(result.size());
        *(Pos*)result.data.data() = p;
        return result;
    }

    static VgCommand fillColor(const Color& c) {
        VgCommand result{EType::FillColor, {}};
        result.data.resize(result.size());
        *(Color*)result.data.data() = c;
        return result;
    }

    static VgCommand strokeColor(const Color& c) {
        VgCommand result{EType::StrokeColor, {}};
        result.data.resize(result.size());
        *(Color*)result.data.data() = c;
        return result;
    }

    static VgCommand fill() { return {EType::Fill, {}}; }
    static VgCommand stroke() { return {EType::Stroke, {}}; }
    static VgCommand beginPath() { return {EType::BeginPath, {}}; }

    EType type;
    std::vector<char> data;
};

TEV_NAMESPACE_END
