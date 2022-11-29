// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

TEV_NAMESPACE_BEGIN

struct VgCommand {
    enum class EType : int8_t {
        Invalid = 127,
        Save = 0,
        Restore = 1,
        FillColor = 2,
        Fill = 3,
        StrokeColor = 4,
        Stroke = 5,
        BeginPath = 6,
        ClosePath = 7,
        PathWinding = 8,
        DebugDumpPathCache = 9,
        MoveTo = 10,
        LineTo = 11,
        ArcTo = 12,
        Arc = 13,
        BezierTo = 14,
        Circle = 15,
        Ellipse = 16,
        QuadTo = 17,
        Rect = 18,
        RoundedRect = 19,
        RoundedRectVarying = 20,
    };

    VgCommand() : type{EType::Invalid} {}
    VgCommand(EType type, const std::vector<float>& data) : type{type}, data{data} {
        if (size() != data.size()) {
            throw std::runtime_error{"VgCommand constructed with invalid amount of data"};
        }
    }

    enum EWinding : int {
        CounterClockwise = 1,
        Clockwise = 2,
    };

    struct Pos { float x, y; };
    struct Size { float width, height; };
    struct Color { float r, g, b, a; };

    // Returns the expected (not actual) size of `data` in number of bytes, depending
    // on the type of the command.
    size_t bytes() const {
        switch (type) {
            case EType::Save: return 0;
            case EType::Restore: return 0;
            case EType::FillColor: return sizeof(Color);
            case EType::Fill: return 0;
            case EType::StrokeColor: return sizeof(Color);
            case EType::Stroke: return 0;
            case EType::BeginPath: return 0;
            case EType::ClosePath: return 0;
            case EType::PathWinding: return sizeof(float);
            case EType::DebugDumpPathCache: return 0;
            case EType::MoveTo: return sizeof(Pos);
            case EType::LineTo: return sizeof(Pos);
            case EType::ArcTo: return sizeof(Pos) * 2 + sizeof(float) /* radius */;
            case EType::Arc: return sizeof(Pos) + sizeof(float) * 4 /* radius, 2 angles, winding */;
            case EType::BezierTo: return sizeof(Pos) * 3 /* 2 control points, end point */;
            case EType::Circle: return sizeof(Pos) + sizeof(float) /* radius */;
            case EType::Ellipse: return sizeof(Pos) + sizeof(Size);
            case EType::QuadTo: return sizeof(Pos) * 2 /* control point, end point */;
            case EType::Rect: return sizeof(Pos) + sizeof(Size);
            case EType::RoundedRect: return sizeof(Pos) + sizeof(Size) + sizeof(float) /* radius */;
            case EType::RoundedRectVarying: return sizeof(Pos) + sizeof(Size) + sizeof(float) * 4 /* radius per corner */;
            default: throw std::runtime_error{"Invalid VgCommand type."};
        }
    }

    // Returns the expected size of `data` in number of floats, depending  on the type of the command.
    size_t size() const {
        return bytes() / sizeof(float);
    }

    static VgCommand save() { return {EType::Save, {}}; }
    static VgCommand restore() { return {EType::Restore, {}}; }

    static VgCommand fillColor(const Color& c) { return {EType::FillColor, {c.r, c.g, c.b, c.a}}; }
    static VgCommand fill() { return {EType::Fill, {}}; }

    static VgCommand strokeColor(const Color& c) { return {EType::StrokeColor, {c.r, c.g, c.b, c.a}}; }
    static VgCommand stroke() { return {EType::Stroke, {}}; }

    static VgCommand beginPath() { return {EType::BeginPath, {}}; }
    static VgCommand closePath() { return {EType::ClosePath, {}}; }
    static VgCommand pathWinding(EWinding winding) { return {EType::PathWinding, {(float)(int)winding}}; }

    static VgCommand moveTo(const Pos& p) { return {EType::MoveTo, {p.x, p.y}}; }
    static VgCommand lineTo(const Pos& p) { return {EType::LineTo, {p.x, p.y}}; }

    static VgCommand arcTo(const Pos& p1, const Pos& p2, float radius) { return {EType::ArcTo, {p1.x, p1.y, p2.x, p2.y, radius}}; }
    static VgCommand arc(const Pos& center, float radius, float angle_begin, float angle_end, EWinding winding) {
        return {EType::Arc, {center.x, center.y, radius, angle_begin, angle_end, (float)(int)winding}};
    }

    static VgCommand bezierTo(const Pos& c1, const Pos& c2, const Pos& p) { return {EType::BezierTo, {c1.x, c1.y, c2.x, c2.y, p.x, p.y}}; }
    static VgCommand circle(const Pos& center, float radius) { return {EType::Circle, {center.x, center.y, radius}}; }
    static VgCommand ellipse(const Pos& center, const Size& radius) { return {EType::Ellipse, {center.x, center.y, radius.width, radius.height}}; }
    static VgCommand quadTo(const Pos& c, const Pos& p) { return {EType::QuadTo, {c.x, c.y, p.x, p.y}}; }
    static VgCommand rect(const Pos& p, const Size& size) { return {EType::Rect, {p.x, p.y, size.width, size.height}}; }
    static VgCommand roundedRect(const Pos& p, const Size& size, float radius) { return {EType::RoundedRect, {p.x, p.y, size.width, size.height, radius}}; }
    static VgCommand roundedRectVarying(const Pos& p, const Size& size, float radiusTopLeft, float radiusTopRight, float radiusBottomRight, float radiusBottomLeft) {
        return {EType::RoundedRectVarying, {p.x, p.y, size.width, size.height, radiusTopLeft, radiusTopRight, radiusBottomRight, radiusBottomLeft}};
    }

    EType type;
    std::vector<float> data;
};

TEV_NAMESPACE_END
