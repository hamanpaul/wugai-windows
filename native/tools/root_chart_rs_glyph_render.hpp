#ifndef BOSHIAMY_ROOT_CHART_RS_GLYPH_RENDER_HPP
#define BOSHIAMY_ROOT_CHART_RS_GLYPH_RENDER_HPP
#include "root_chart_q7_render.hpp"
#include "root_chart_rs_glyphs.hpp"
namespace boshiamy {
inline bool draw_root_chart_rs_glyph(HDC dc, wchar_t token, RECT box) {
    for (const auto& glyph : root_chart_rs_ink_glyphs) {
        if (glyph.token != token) continue;
        draw_root_chart_source_ink_glyph(
            dc, box, glyph.width, glyph.height,
            root_chart_rs_ink_runs.data() + glyph.first_run, glyph.run_count);
        return true;
    }
    return false;
}
}
#endif
