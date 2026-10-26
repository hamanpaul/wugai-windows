#ifndef BOSHIAMY_TSF_MODE_ICON_HPP
#define BOSHIAMY_TSF_MODE_ICON_HPP

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace boshiamy::mode_icon {

// The state is the product's common conversion toggle. Chinese means the IME
// is open; English means it is closed. Width/shape modes are intentionally not
// represented by this system-tray mode glyph.
enum class State { Chinese, English };

struct Pixel { std::uint8_t b, g, r, a; };
struct RenderStats {
    std::uint8_t source_peak = 0;
    std::uint8_t output_glyph_coverage_peak = 0;
    std::uint8_t output_alpha_peak = 0;
};

namespace detail {
struct RectD { double left, top, right, bottom; };

inline bool inside(const RectD &rect, double x, double y) noexcept {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

inline double distance_to_rect(const RectD &rect, double x, double y) noexcept {
    const double dx = x < rect.left ? rect.left - x : (x > rect.right ? x - rect.right : 0.0);
    const double dy = y < rect.top ? rect.top - y : (y > rect.bottom ? y - rect.bottom : 0.0);
    return std::sqrt(dx * dx + dy * dy);
}

// Draw the observed Chinese mode symbol as a vector 中: one continuous
// center stem crossing a rectangular frame. The white path is geometry; the
// black outline is a one-logical-pixel distance expansion with antialiased
// coverage. This keeps the product implementation independent of the oracle's
// captured pixel array.
inline bool render_chinese_vector(int width, int height, std::vector<Pixel> *pixels,
    RenderStats *stats) {
    const double unit = static_cast<double>(std::min(width, height)) / 16.0;
    const double origin_x = (width - 16.0 * unit) / 2.0;
    const double origin_y = (height - 16.0 * unit) / 2.0;
    auto rect = [=](double left, double top, double right, double bottom) {
        return RectD{origin_x + left * unit, origin_y + top * unit,
            origin_x + right * unit, origin_y + bottom * unit};
    };
    const std::array<RectD, 5> strokes = {{
        rect(8, 1, 9, 15),
        rect(3, 4, 14, 5),
        rect(3, 9, 14, 10),
        rect(3, 5, 4, 9),
        rect(13, 5, 14, 9),
    }};
    constexpr int samples_per_axis = 8;
    constexpr int sample_count = samples_per_axis * samples_per_axis;
    const double outline_radius = unit;
    try { pixels->assign(static_cast<size_t>(width) * height, Pixel{0, 0, 0, 0}); }
    catch (...) { return false; }
    if (stats) stats->source_peak = 255;

    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        unsigned glyph_samples = 0, outline_samples = 0;
        for (int sy = 0; sy < samples_per_axis; ++sy) for (int sx = 0; sx < samples_per_axis; ++sx) {
            const double px = x + (sx + 0.5) / samples_per_axis;
            const double py = y + (sy + 0.5) / samples_per_axis;
            bool in_glyph = false;
            double nearest = outline_radius + 1.0;
            for (const RectD &stroke : strokes) {
                if (inside(stroke, px, py)) { in_glyph = true; break; }
                nearest = std::min(nearest, distance_to_rect(stroke, px, py));
            }
            if (in_glyph) ++glyph_samples;
            else if (nearest <= outline_radius) ++outline_samples;
        }

        const unsigned glyph_alpha = (glyph_samples * 255u + sample_count / 2u) / sample_count;
        const unsigned outline_coverage = (outline_samples * 255u + sample_count / 2u) / sample_count;
        // Microsoft describes a half-alpha outline. Preserve a soft AA corner
        // (the official icon has 75/255 at its diagonal tips) while keeping
        // straight edge pixels at the specified 128/255 outline opacity.
        const unsigned outline_alpha = outline_coverage >= 230u ? 128u :
            outline_coverage >= 115u ? 75u : (outline_coverage * 128u + 127u) / 255u;
        const unsigned alpha = glyph_alpha + outline_alpha * (255u - glyph_alpha) / 255u;
        const std::uint8_t channel = static_cast<std::uint8_t>(glyph_alpha);
        (*pixels)[static_cast<size_t>(y) * width + x] = {channel, channel, channel,
            static_cast<std::uint8_t>(alpha)};
        if (stats) {
            stats->output_glyph_coverage_peak = std::max<std::uint8_t>(
                stats->output_glyph_coverage_peak, channel);
            stats->output_alpha_peak = std::max<std::uint8_t>(
                stats->output_alpha_peak, static_cast<std::uint8_t>(alpha));
        }
    }
    return true;
}

struct SegmentD { double x1, y1, x2, y2, half_width; };
struct QuadraticStrokeD {
    double x0, y0, cx, cy, x1, y1;
    double half_start, half_control, half_end;
};
inline double distance_to_segment(const SegmentD &segment, double x, double y) noexcept {
    const double dx = segment.x2 - segment.x1, dy = segment.y2 - segment.y1;
    const double length_squared = dx * dx + dy * dy;
    const double t = length_squared == 0.0 ? 0.0 : std::clamp(
        ((x - segment.x1) * dx + (y - segment.y1) * dy) / length_squared, 0.0, 1.0);
    const double px = segment.x1 + t * dx, py = segment.y1 + t * dy;
    return std::max(0.0, std::sqrt((x - px) * (x - px) + (y - py) * (y - py)) - segment.half_width);
}

inline double distance_to_tapered_quadratic(const QuadraticStrokeD &stroke,
    double x, double y) noexcept {
    constexpr int steps = 16;
    double nearest = 1e9, px = stroke.x0, py = stroke.y0;
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps, u = 1.0 - t;
        const double nx = u * u * stroke.x0 + 2.0 * u * t * stroke.cx + t * t * stroke.x1;
        const double ny = u * u * stroke.y0 + 2.0 * u * t * stroke.cy + t * t * stroke.y1;
        const double mt = (static_cast<double>(i) - 0.5) / steps, mu = 1.0 - mt;
        const double half = mu * mu * stroke.half_start +
            2.0 * mu * mt * stroke.half_control + mt * mt * stroke.half_end;
        nearest = std::min(nearest,
            distance_to_segment({px, py, nx, ny, half}, x, y));
        px = nx;
        py = ny;
    }
    return nearest;
}

// Source-derived strokes for the observed closed-mode glyph 英: the 艹 top
// radical, its paired short stems, the central 央 body, and its two lower
// diagonals. Keep these as scalable paths rather than a copied 16x16 raster.
inline bool render_english_vector(int width, int height, std::vector<Pixel> *pixels,
    RenderStats *stats) {
    const double unit = static_cast<double>(std::min(width, height)) / 16.0;
    const double origin_x = (width - 16.0 * unit) / 2.0;
    const double origin_y = (height - 16.0 * unit) / 2.0;
    auto rect = [=](double left, double top, double right, double bottom) {
        return RectD{origin_x + left * unit, origin_y + top * unit,
            origin_x + right * unit, origin_y + bottom * unit};
    };
    const std::array<RectD, 8> bars = {{
        rect(2, 3, 15, 4), rect(6, 2, 7, 5), rect(10, 2, 11, 5),
        rect(4, 6, 13, 7), rect(4, 7, 5, 10), rect(8, 5, 9, 11),
        rect(12, 7, 13, 10), rect(2, 10, 15, 11),
    }};
    const SegmentD left_hook = {
        // The left hooked stroke joins the center stem in the lower body.
        origin_x + 5.15 * unit, origin_y + 7.15 * unit,
        origin_x + 5.45 * unit, origin_y + 8.7 * unit, 0.20 * unit};
    // Reconstructed from the captured closed-mode symbol's lower stroke
    // silhouette: the two feet spread from the shared body to either side.
    const std::array<QuadraticStrokeD, 2> legs = {{
        {origin_x + 8.0 * unit, origin_y + 11.0 * unit,
         origin_x + 7.5 * unit, origin_y + 13.0 * unit,
         origin_x + 2.3 * unit, origin_y + 14.4 * unit,
         0.45 * unit, 0.45 * unit, 0.40 * unit},
        {origin_x + 8.0 * unit, origin_y + 11.0 * unit,
         origin_x + 8.7 * unit, origin_y + 13.0 * unit,
         origin_x + 14.2 * unit, origin_y + 14.4 * unit,
         0.45 * unit, 0.45 * unit, 0.45 * unit},
    }};
    constexpr int samples_per_axis = 8;
    constexpr int sample_count = samples_per_axis * samples_per_axis;
    const double outline_radius = unit;
    try { pixels->assign(static_cast<size_t>(width) * height, Pixel{0, 0, 0, 0}); }
    catch (...) { return false; }
    if (stats) stats->source_peak = 255;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        unsigned glyph_samples = 0, soft_glyph_samples = 0, outline_samples = 0;
        for (int sy = 0; sy < samples_per_axis; ++sy) for (int sx = 0; sx < samples_per_axis; ++sx) {
            const double px = x + (sx + 0.5) / samples_per_axis;
            const double py = y + (sy + 0.5) / samples_per_axis;
            bool in_glyph = false;
            bool in_soft_glyph = false;
            double nearest = outline_radius + 1.0;
            for (const RectD &stroke : bars) {
                if (inside(stroke, px, py)) { in_glyph = true; break; }
                nearest = std::min(nearest, distance_to_rect(stroke, px, py));
            }
            if (!in_glyph) {
                const double distance = distance_to_segment(left_hook, px, py);
                if (distance <= 0.0) in_glyph = true;
                else nearest = std::min(nearest, distance);
            }
            if (!in_glyph) for (const QuadraticStrokeD &leg : legs) {
                const double distance = distance_to_tapered_quadratic(leg, px, py);
                if (distance <= 0.0) { in_soft_glyph = true; break; }
                nearest = std::min(nearest, distance);
            }
            if (in_glyph) ++glyph_samples;
            else if (in_soft_glyph) ++soft_glyph_samples;
            else if (nearest <= outline_radius) ++outline_samples;
        }
        const unsigned glyph_alpha = (glyph_samples * 255u + soft_glyph_samples * 250u + sample_count / 2u) / sample_count;
        const unsigned outline_coverage = (outline_samples * 255u + sample_count / 2u) / sample_count;
        const unsigned outline_alpha = outline_coverage >= 230u ? 128u :
            outline_coverage >= 115u ? 75u : (outline_coverage * 128u + 127u) / 255u;
        const unsigned alpha = glyph_alpha + outline_alpha * (255u - glyph_alpha) / 255u;
        const std::uint8_t channel = static_cast<std::uint8_t>(glyph_alpha);
        (*pixels)[static_cast<size_t>(y) * width + x] = {channel, channel, channel,
            static_cast<std::uint8_t>(alpha)};
        if (stats) {
            stats->output_glyph_coverage_peak = std::max<std::uint8_t>(stats->output_glyph_coverage_peak, channel);
            stats->output_alpha_peak = std::max<std::uint8_t>(stats->output_alpha_peak, static_cast<std::uint8_t>(alpha));
        }
    }
    return true;
}
} // namespace detail

inline const wchar_t *glyph(State state) noexcept {
    // The official closed-state capture is the localized Chinese glyph 英,
    // not a Latin A. The renderer below represents its observed strokes as
    // scalable vector paths rather than copying the captured pixel array.
    return state == State::Chinese ? L"中" : L"英";
}

// Renders the Microsoft-documented white typographic glyph with a one-pixel
// half-alpha black outline. Supersampling keeps the small tray rendition
// legible. This returns premultiplied-alpha BGRA pixels for CreateIconIndirect.
inline bool render(State state, int width, int height, std::vector<Pixel> *pixels,
    RenderStats *stats = nullptr) {
    if (!pixels || width < 8 || height < 8 || width > 256 || height > 256) return false;
    pixels->clear();
    if (stats) *stats = {};
    if (state == State::Chinese) return detail::render_chinese_vector(width, height, pixels, stats);
    return detail::render_english_vector(width, height, pixels, stats);
}

inline HICON create(State state, int width = 0, int height = 0) noexcept {
    if (!width) width = GetSystemMetrics(SM_CXSMICON);
    if (!height) height = GetSystemMetrics(SM_CYSMICON);
    std::vector<Pixel> pixels;
    if (!render(state, width, height, &pixels)) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    HBITMAP color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) { if (color) DeleteObject(color); DeleteDC(dc); return nullptr; }
    std::copy(pixels.begin(), pixels.end(), static_cast<Pixel *>(bits));
    const size_t mask_stride = ((static_cast<size_t>(width) + 31u) / 32u) * 4u;
    std::vector<std::uint8_t> and_bits;
    try { and_bits.assign(mask_stride * height, 0); }
    catch (...) { DeleteObject(color); DeleteDC(dc); return nullptr; }
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        if (pixels[static_cast<size_t>(y) * width + x].a == 0)
            and_bits[static_cast<size_t>(y) * mask_stride + static_cast<size_t>(x / 8)] |=
                static_cast<std::uint8_t>(0x80u >> (x % 8));
    }
    struct MonochromeBitmapInfo {
        BITMAPINFOHEADER header;
        RGBQUAD colors[2];
    } mask_info{};
    mask_info.header.biSize = sizeof(BITMAPINFOHEADER);
    mask_info.header.biWidth = width;
    mask_info.header.biHeight = -height;
    mask_info.header.biPlanes = 1;
    mask_info.header.biBitCount = 1;
    mask_info.header.biCompression = BI_RGB;
    mask_info.colors[0] = {0, 0, 0, 0};
    mask_info.colors[1] = {255, 255, 255, 0};
    void *mask_bits = nullptr;
    HBITMAP mask = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&mask_info),
        DIB_RGB_COLORS, &mask_bits, nullptr, 0);
    if (mask && mask_bits) std::copy(and_bits.begin(), and_bits.end(), static_cast<std::uint8_t *>(mask_bits));
    else if (mask) { DeleteObject(mask); mask = nullptr; }
    if (!mask) { DeleteObject(color); DeleteDC(dc); return nullptr; }
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

} // namespace boshiamy::mode_icon

#endif
