// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/VectorGraphics.h>

#include <nanovg.h>

using namespace std;

TEV_NAMESPACE_BEGIN

size_t VgCommand::size() const {
    switch (type) {
        case EType::Save: return 0;
        case EType::Restore: return 0;
        case EType::MoveTo: return sizeof(Pos);
        case EType::LineTo: return sizeof(Pos);
        case EType::FillColor: return sizeof(Color);
        case EType::StrokeColor: return sizeof(Color);
        case EType::Fill: return 0;
        default: throw runtime_error{"Invalid VgCommand type."};
    }
}

void VgCommand::apply(NVGcontext* ctx) const {
    switch (type) {
        case EType::Save: nvgSave(ctx); return;
        case EType::Restore: nvgRestore(ctx); return;
        case EType::MoveTo: {
            const Pos& p = *reinterpret_cast<const Pos*>(data.data());
            nvgMoveTo(ctx, p.x, p.y);
        } return;
        case EType::LineTo: {
            const Pos& p = *reinterpret_cast<const Pos*>(data.data());
            nvgLineTo(ctx, p.x, p.y);
        } return;
        case EType::FillColor: {
            const Color& c = *reinterpret_cast<const Color*>(data.data());
            nvgFillColor(ctx, {c.r, c.g, c.b, c.a});
        } return;
        case EType::StrokeColor: {
            const Color& c = *reinterpret_cast<const Color*>(data.data());
            nvgStrokeColor(ctx, {c.r, c.g, c.b, c.a});
        } return;
        case EType::Fill: nvgFill(ctx); return;
        default: throw runtime_error{"Invalid VgCommand type."};
    }
}

TEV_NAMESPACE_END
