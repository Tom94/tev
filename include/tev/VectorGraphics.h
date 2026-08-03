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

#include <tev/Common.h>

#include <span>

namespace tev {

struct VgCommand {
    enum class EType : int8_t {
        Invalid = 127,
        Save = 0,
        Restore = 1,
        FillColor = 2,
        Fill = 3,
        StrokeColor = 4,
        StrokeWidth = 25,
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
        Text = 21,
        TextAlign = 22,
        FontFace = 23,
        FontSize = 24,
    };

    VgCommand(EType type = EType::Invalid, std::span<const float> data = {}, std::string_view stringData = "") :
        type{type}, data{data.begin(), data.end()}, stringData{stringData} {
        if (size() != data.size()) {
            throw std::runtime_error{"VgCommand constructed with invalid amount of data"};
        }
    }

    enum EScaleKind : int {
        Relative = 0,
        Absolute = 1,
    };

    enum ETextAlign : int {
        Left = 1 << 0,
        Center = 1 << 1,
        Right = 1 << 2,
        Top = 1 << 3,
        Middle = 1 << 4,
        Bottom = 1 << 5,
        Baseline = 1 << 6
    };

    enum EWinding : int {
        CounterClockwise = 1,
        Clockwise = 2,
    };

    struct Pos {
        float x, y;
    };

    struct Size {
        float width, height;
    };

    struct Color {
        float r, g, b, a;
    };

    bool hasStringData() const { return type == EType::Text || type == EType::FontFace; }

    // Returns the expected (not actual) size of `data` in number of bytes, depending on the type of the command.
    // For EType::Text, this counts only the fixed portion (position, font size). The string
    // payload itself is carried separately in `text` and is not included in this count.
    size_t bytes() const {
        switch (type) {
            case EType::Invalid: return 0;
            case EType::Save: return 0;
            case EType::Restore: return 0;
            case EType::FillColor: return sizeof(Color);
            case EType::Fill: return 0;
            case EType::StrokeColor: return sizeof(Color);
            case EType::StrokeWidth: return sizeof(float) /* width */ + sizeof(float) /* absolute or relative */;
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
            case EType::Text: return sizeof(Pos); // + string payload
            case EType::TextAlign: return sizeof(float);
            case EType::FontFace: return 0; // + string payload
            case EType::FontSize: return sizeof(float) /* size */ + sizeof(float) /* absolute or relative */;
            default: throw std::runtime_error{"Invalid VgCommand type."};
        }
    }

    // Returns the expected size of `data` in number of floats, depending  on the type of the command.
    size_t size() const { return bytes() / sizeof(float); }

    static VgCommand save() { return {EType::Save, {}}; }
    static VgCommand restore() { return {EType::Restore, {}}; }

    static VgCommand fillColor(Color c) { return {EType::FillColor, {{c.r, c.g, c.b, c.a}}}; }
    static VgCommand fill() { return {EType::Fill, {}}; }

    static VgCommand strokeColor(Color c) { return {EType::StrokeColor, {{c.r, c.g, c.b, c.a}}}; }
    static VgCommand strokeWidth(float width, EScaleKind scaleKind) { return {EType::StrokeWidth, {{width, (float)(int)scaleKind}}}; }
    static VgCommand stroke() { return {EType::Stroke, {}}; }

    static VgCommand beginPath() { return {EType::BeginPath, {}}; }
    static VgCommand closePath() { return {EType::ClosePath, {}}; }
    static VgCommand pathWinding(EWinding winding) { return {EType::PathWinding, {{(float)(int)winding}}}; }

    static VgCommand moveTo(Pos p) { return {EType::MoveTo, {{p.x, p.y}}}; }

    static VgCommand lineTo(Pos p) { return {EType::LineTo, {{p.x, p.y}}}; }

    static VgCommand arcTo(Pos p1, Pos p2, float radius) { return {EType::ArcTo, {{p1.x, p1.y, p2.x, p2.y, radius}}}; }

    static VgCommand arc(Pos center, float radius, float angle_begin, float angle_end, EWinding winding) {
        return {EType::Arc, {{center.x, center.y, radius, angle_begin, angle_end, (float)(int)winding}}};
    }

    static VgCommand bezierTo(Pos c1, Pos c2, Pos p) { return {EType::BezierTo, {{c1.x, c1.y, c2.x, c2.y, p.x, p.y}}}; }

    static VgCommand circle(Pos center, float radius) { return {EType::Circle, {{center.x, center.y, radius}}}; }

    static VgCommand ellipse(Pos center, Size radius) { return {EType::Ellipse, {{center.x, center.y, radius.width, radius.height}}}; }

    static VgCommand quadTo(Pos c, Pos p) { return {EType::QuadTo, {{c.x, c.y, p.x, p.y}}}; }

    static VgCommand rect(Pos p, Size size) { return {EType::Rect, {{p.x, p.y, size.width, size.height}}}; }

    static VgCommand roundedRect(Pos p, Size size, float radius) {
        return {EType::RoundedRect, {{p.x, p.y, size.width, size.height, radius}}};
    }

    static VgCommand
        roundedRectVarying(Pos p, Size size, float radiusTopLeft, float radiusTopRight, float radiusBottomRight, float radiusBottomLeft) {
        return {
            EType::RoundedRectVarying,
            {{p.x, p.y, size.width, size.height, radiusTopLeft, radiusTopRight, radiusBottomRight, radiusBottomLeft}}
        };
    }

    static VgCommand text(Pos p, std::string_view str) { return {EType::Text, {{p.x, p.y}}, std::string{str}}; }

    static VgCommand textAlign(ETextAlign align) { return {EType::TextAlign, {{(float)(int)align}}}; }

    static VgCommand fontFace(std::string_view face) { return {EType::FontFace, {}, face}; }

    static VgCommand fontSize(float fontSize, EScaleKind sizeKind) { return {EType::FontSize, {{fontSize, (float)(int)sizeKind}}}; }

    EType type;
    std::vector<float> data;
    std::string stringData;
};

} // namespace tev
