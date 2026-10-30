#ifndef BOSHIAMY_CANDIDATE_WINDOW_HPP
#define BOSHIAMY_CANDIDATE_WINDOW_HPP

#include <windows.h>

#include "../engine.hpp"
#include "../composition_display.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <mutex>
#include <cstring>
#include <vector>

namespace boshiamy {

// Some MinGW SDK versions expose these only when _WIN32_WINNT is set to
// Windows 8; keep the documented WinEvent values available to this header.
inline constexpr DWORD candidate_ime_show_event = 0x8027;
inline constexpr DWORD candidate_ime_hide_event = 0x8028;
inline constexpr DWORD candidate_ime_change_event = 0x8029;

// A non-activating, owned Win32 popup for TSF composition and candidate UI.
// Coordinates passed to update() are physical screen pixels. The owner should
// be the HWND obtained from ITfContextView::GetWnd (or the focused HWND fallback).
class CandidateWindow {
public:
    static constexpr int viewport_control_id = 0x4201;
#ifdef BOSHIAMY_CANDIDATE_WINDOW_TESTING
    using TestNotifySink = void (WINAPI*)(DWORD, HWND, LONG, LONG);
    static void set_test_notify_sink(TestNotifySink sink) { test_notify_sink_ = sink; }
#endif

    explicit CandidateWindow(HWND owner = nullptr) : owner_(owner) {
        if (!acquire_class(module_instance_)) return;
        class_ref_ = true;
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            class_name(), L"嘸蝦米候選視窗", WS_POPUP | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1, 1, owner_, nullptr,
            module_instance_, this);
        if (!hwnd_) { destroy(); return; }
        SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_));
        composition_label_ = create_label(hwnd_);
        viewport_ = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(viewport_control_id)),
            module_instance_, nullptr);
        if (viewport_) {
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR previous = SetWindowLongPtrW(viewport_, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&viewport_proc));
            if (!previous && GetLastError() != ERROR_SUCCESS) {
                DestroyWindow(viewport_);
                viewport_ = nullptr;
            } else {
                viewport_proc_ = reinterpret_cast<WNDPROC>(previous);
            }
        }
        for (std::size_t i = 0; i < candidate_labels_.size(); ++i) {
            candidate_labels_[i] = viewport_ ? create_label(viewport_) : nullptr;
            if (!candidate_labels_[i]) continue;
            SetLastError(ERROR_SUCCESS);
            const LONG_PTR previous = SetWindowLongPtrW(candidate_labels_[i], GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&candidate_label_proc));
            if (!previous && GetLastError() != ERROR_SUCCESS) {
                DestroyWindow(candidate_labels_[i]);
                candidate_labels_[i] = nullptr;
            } else {
                candidate_label_procs_[i] = reinterpret_cast<WNDPROC>(previous);
            }
        }
        if (!composition_label_ || !viewport_ ||
            std::any_of(candidate_labels_.begin(), candidate_labels_.end(), [](HWND h) { return h == nullptr; })) {
            destroy();
        }
    }

    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;

    ~CandidateWindow() { destroy(); }

    HWND hwnd() const { return hwnd_; }
    HWND viewport_hwnd() const { return viewport_; }
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_) != FALSE; }
    HWND owner() const { return owner_; }

    void update(const Outcome& outcome, const RECT& anchor, UINT dpi, const RECT& monitor_workarea) {
        if (!hwnd_) return;
        const bool learning_hint = outcome.composition.empty() && !outcome.hint.empty();
        if ((!learning_hint && outcome.composition.empty()) || dpi == 0 || !valid_anchor(anchor) || !valid_workarea(monitor_workarea)) {
            hide();
            return;
        }
        if (!learning_hint) {
            update_official_candidates(outcome, anchor, dpi, monitor_workarea);
            return;
        }
        normal_candidate_mode_ = false;
        selected_row_ = 0;

        const int scale = static_cast<int>(dpi);
        const int margin = scaled(6, scale);
        const int border = 1; // One physical pixel at every monitor scale.
        const int work_width = monitor_workarea.right - monitor_workarea.left;
        const int work_height = monitor_workarea.bottom - monitor_workarea.top;
        if (work_width <= 0 || work_height <= 0) { hide(); return; }
        const std::size_t count = learning_hint ? 0 : outcome.candidates.size();
        std::wstring composition = utf8_to_wide(learning_hint ? outcome.hint : composition_display(outcome.composition));
        if (composition.empty()) { hide(); return; }
        if (learning_hint) {
            for (auto& letter : composition) if (letter >= L'a' && letter <= L'z') letter -= L'a' - L'A';
            const wchar_t* label=outcome.hint_kind==HintKind::Recommended?L"建議碼：":outcome.hint_kind==HintKind::All?L"所有拆碼：":L"最簡碼：";
            composition = label + composition;
        }
        update_font(scale, false);
        std::wstring title = learning_hint ? L"嘸蝦米拆碼提示：" + composition : L"嘸蝦米組字：" + composition;
        SetWindowTextW(hwnd_, title.c_str());
        SetWindowTextW(composition_label_, composition.c_str());

        const std::size_t visible_count = std::min<std::size_t>(count, candidate_labels_.size());
        std::vector<std::wstring> candidate_lines;
        candidate_lines.reserve(visible_count);
        for (std::size_t i = 0; i < visible_count; ++i) {
            candidate_lines.push_back(std::to_wstring(i) + L" " + utf8_to_wide(outcome.candidates[i].text));
            SetWindowTextW(candidate_labels_[i], candidate_lines.back().c_str());
        }
        for (std::size_t i = visible_count; i < candidate_labels_.size(); ++i) {
            SetWindowTextW(candidate_labels_[i], L"");
            ShowWindow(candidate_labels_[i], SW_HIDE);
        }

        const int max_width = std::min(scaled(320, scale), work_width);
        int natural_width = measure_text_width(composition);
        for (const auto& line : candidate_lines) natural_width = std::max(natural_width, measure_text_width(line));
        const int min_width = std::min(max_width, scaled(72, scale));
        const int scrollbar_width = scrollbar_width_for_dpi(scale);
        const int line_padding = scaled(3, scale);
        const std::wstring pagination_state = learning_hint ? L"hint" : std::to_wstring(outcome.page);
        const std::wstring signature = make_scroll_signature(composition, pagination_state, candidate_lines);
        if (signature != scroll_signature_) {
            scroll_signature_ = signature;
            scroll_offset_ = 0;
        }
        int width = std::min(max_width, std::max(min_width, natural_width + margin * 2 + border * 2));
        bool has_scrollbar = false;
        int inner = 1;
        int header_height = 0;
        std::vector<int> candidate_heights;
        int candidate_total_height = 0;
        auto measure_rows = [&]() {
            const int bar_reserve = has_scrollbar ? scrollbar_width : 0;
            inner = std::max(1, width - margin * 2 - border * 2 - bar_reserve);
            header_height = measure_text_height(composition, inner) + line_padding * 2;
            candidate_heights.clear();
            candidate_total_height = 0;
            for (const auto& line : candidate_lines) {
                const int row_height = measure_candidate_height(line, inner) + line_padding * 2;
                candidate_heights.push_back(row_height);
                candidate_total_height += row_height;
            }
        };
        measure_rows();
        const int required_height = border * 2 + margin * 2 + header_height + candidate_total_height;
        has_scrollbar = candidate_total_height > 0 && required_height > work_height;
        if (has_scrollbar) {
            width = std::min(max_width, width + scrollbar_width);
            measure_rows();
        }
        const int content_height = border * 2 + margin * 2 + header_height + candidate_total_height;
        const int height = std::min(std::max(scaled(48, scale), content_height), work_height);

        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        const bool currently_has_scrollbar = (style & WS_VSCROLL) != 0;
        if (currently_has_scrollbar != has_scrollbar) {
            if (has_scrollbar) style |= WS_VSCROLL;
            else style &= ~static_cast<LONG_PTR>(WS_VSCROLL);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }

        const auto origin = popup_origin(anchor, dpi, monitor_workarea, width, height, false);
        const int x = origin.x;
        const int top = origin.top;
        const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
        RECT previous_bounds{};
        const bool had_previous_bounds = was_visible && GetWindowRect(hwnd_, &previous_bounds) != FALSE;
        if (!SetWindowPos(hwnd_, HWND_TOPMOST, x, top, width, height,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return;
        const int header_top = border + margin;
        const int viewport_top = header_top + header_height;
        const int viewport_bottom = std::max(viewport_top, height - border - margin);
        const int viewport_height = viewport_bottom - viewport_top;
        candidate_inner_width_ = inner;
        candidate_x_ = border + margin;
        candidate_viewport_top_ = viewport_top;
        candidate_viewport_bottom_ = viewport_bottom;
        candidate_viewport_height_ = viewport_height;
        candidate_heights_ = candidate_heights;
        candidate_total_height_ = candidate_total_height;
        candidate_scroll_max_ = std::max(0, candidate_total_height_ - viewport_height);
        scroll_offset_ = std::clamp(scroll_offset_, 0, candidate_scroll_max_);
        if (has_scrollbar && candidate_scroll_max_ > 0) {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            info.nMin = 0;
            info.nMax = std::max(0, candidate_total_height_ - 1);
            info.nPage = static_cast<UINT>(std::max(1, viewport_height));
            info.nPos = scroll_offset_;
            SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
        } else {
            candidate_scroll_max_ = 0;
            scroll_offset_ = 0;
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            info.nMin = 0;
            info.nMax = 0;
            info.nPage = 0;
            info.nPos = 0;
            SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
        }
        MoveWindow(composition_label_, candidate_x_, header_top, inner, header_height, TRUE);
        ShowWindow(composition_label_, SW_SHOWNOACTIVATE);
        MoveWindow(viewport_, candidate_x_, viewport_top, inner, viewport_height, TRUE);
        layout_candidate_rows();
        SetWindowPos(composition_label_, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (!was_visible) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (!IsWindowVisible(hwnd_)) return;

        RECT current_bounds{};
        const bool has_current_bounds = GetWindowRect(hwnd_, &current_bounds) != FALSE;
        if (!ime_visible_) {
            ime_visible_ = true;
            notify_ime_event(candidate_ime_show_event, hwnd_);
        } else if (was_visible && had_previous_bounds && has_current_bounds &&
                   !same_rect(previous_bounds, current_bounds)) {
            notify_ime_event(candidate_ime_change_event, hwnd_);
        }
    }

    void hide() {
        if (!hwnd_) return;
        scroll_offset_ = 0;
        wheel_delta_remainder_ = 0;
        SetScrollPos(hwnd_, SB_VERT, 0, TRUE);
        if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        if (ime_visible_ && !IsWindowVisible(hwnd_)) {
            ime_visible_ = false;
            notify_ime_event(candidate_ime_hide_event, hwnd_);
        }
    }

    // Called by the TSF owner when its context loses focus; popup HWNDs using
    // WS_EX_NOACTIVATE do not receive the owner's focus notifications themselves.
    void on_focus_lost() { hide(); }

    void destroy() {
        if (hwnd_ && IsWindow(hwnd_)) {
            hide();
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
        composition_label_ = nullptr;
        viewport_ = nullptr;
        viewport_proc_ = nullptr;
        candidate_labels_.fill(nullptr);
        candidate_label_procs_.fill(nullptr);
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        release_class();
    }

private:
    void update_official_candidates(const Outcome& outcome, const RECT& anchor, UINT dpi,
                                    const RECT& workarea) {
        normal_candidate_mode_ = true;
        update_font(static_cast<int>(dpi), true);
        const std::wstring composition = utf8_to_wide(composition_display(outcome.composition));
        SetWindowTextW(hwnd_, (L"嘸蝦米組字：" + composition).c_str());
        SetWindowTextW(composition_label_, composition.c_str());
        ShowWindow(composition_label_, SW_HIDE);

        const std::size_t count = std::min(outcome.candidates.size(), candidate_labels_.size());
        const std::size_t page_start = outcome.page < outcome.candidate_page_indices.size()
            ? outcome.candidate_page_indices[outcome.page] : outcome.page * 10;
        selected_row_ = outcome.candidate_selection >= page_start &&
            outcome.candidate_selection - page_start < count
            ? outcome.candidate_selection - page_start : 0;
        std::vector<std::wstring> rows;
        rows.reserve(count);
        int text_width = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::wstring candidate = utf8_to_wide(candidate_display_text(outcome.candidates[i]));
            rows.push_back(std::to_wstring(i) + L" " + candidate);
            SetWindowTextW(candidate_labels_[i], rows.back().c_str());
        }
        const auto& width_candidates = outcome.layout_candidates
            ? *outcome.layout_candidates : outcome.candidates;
        for (const Candidate& entry : width_candidates)
            text_width = std::max(text_width, measure_text_width(utf8_to_wide(candidate_display_text(entry))));
        for (std::size_t i = count; i < candidate_labels_.size(); ++i) {
            SetWindowTextW(candidate_labels_[i], L"");
            ShowWindow(candidate_labels_[i], SW_HIDE);
        }

        const int scale = static_cast<int>(dpi);
        const int border = std::max(1, scaled(3, scale));
        const int row_height = std::max(1, scaled(24, scale));
        const int gutter = std::max(1, scaled(34, scale));
        const int work_width = workarea.right - workarea.left;
        const int work_height = workarea.bottom - workarea.top;
        const int slots = std::max(outcome.page > 0 ? 10 : 5, static_cast<int>(count));
        const bool fast_code_preview = outcome.mode == Mode::Fast && outcome.query_preview &&
                                       outcome.query == QueryStage::None;
        const int minimum_width = scaled(fast_code_preview ? 72 : 91, scale);
        const int width = std::min(work_width, std::max(minimum_width,
                              std::min(scaled(320, scale), scaled(72, scale) + text_width)));
        auto measure_rows = [&](int available_width) {
            candidate_heights_.clear();
            candidate_total_height_ = 0;
            const int candidate_text_width = std::max(1, available_width - border * 2 - gutter - scaled(1, scale));
            for (std::size_t i = 0; i < count; ++i) {
                const std::wstring candidate = utf8_to_wide(candidate_display_text(outcome.candidates[i]));
                const int measured = measure_text_width(candidate) > candidate_text_width
                    ? measure_text_height(candidate, candidate_text_width) + scaled(2, scale) : row_height;
                const int measured_row = std::max(row_height, measured);
                candidate_heights_.push_back(measured_row);
                candidate_total_height_ += measured_row;
            }
        };
        measure_rows(width);
        const int height = std::min(work_height,
            border * 2 + std::max(slots * row_height, candidate_total_height_) - 1);
        if (width <= border * 2 || height <= border * 2) { hide(); return; }

        const std::wstring signature = make_scroll_signature(composition,
            std::to_wstring(outcome.page), rows);
        if (signature != scroll_signature_) {
            scroll_signature_ = signature;
            scroll_offset_ = 0;
        }
        const int viewport_height = height - border * 2;
        // The final one-pixel row tail is blank padding under the official
        // three-pixel frame, so it must not create a scrollbar for ten rows.
        candidate_scroll_max_ = std::max(0, candidate_total_height_ - viewport_height - 1);
        const bool has_scrollbar = candidate_scroll_max_ > 0;
        if (has_scrollbar) {
            measure_rows(width - scrollbar_width_for_dpi(dpi));
            candidate_scroll_max_ = std::max(0, candidate_total_height_ - viewport_height - 1);
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0, candidate_scroll_max_);
        candidate_x_ = border;
        candidate_inner_width_ = width - border * 2;
        candidate_viewport_top_ = border;
        candidate_viewport_bottom_ = height - border;
        candidate_viewport_height_ = viewport_height;
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        if (((style & WS_VSCROLL) != 0) != has_scrollbar) {
            if (has_scrollbar) style |= WS_VSCROLL;
            else style &= ~static_cast<LONG_PTR>(WS_VSCROLL);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        SCROLLINFO scroll{};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        scroll.nMin = 0;
        scroll.nMax = has_scrollbar ? candidate_total_height_ - 1 : 0;
        scroll.nPage = has_scrollbar ? static_cast<UINT>(viewport_height) : 0;
        scroll.nPos = scroll_offset_;
        SetScrollInfo(hwnd_, SB_VERT, &scroll, TRUE);
        const auto origin = popup_origin(anchor, dpi, workarea, width, height, true);
        const int x = origin.x;
        const int top = origin.top;
        const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
        RECT previous{};
        const bool had_previous = was_visible && GetWindowRect(hwnd_, &previous) != FALSE;
        if (!SetWindowPos(hwnd_, HWND_TOPMOST, x, top, width, height,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return;
        if (has_scrollbar) candidate_inner_width_ = std::max(1, candidate_inner_width_ - scrollbar_width_for_dpi(dpi));
        MoveWindow(viewport_, border, border, candidate_inner_width_, viewport_height, TRUE);
        layout_candidate_rows();
        // Selection can change while the labels keep identical text and size;
        // invalidating only the parent does not repaint their custom highlights.
        for(std::size_t i=0;i<count;++i)InvalidateRect(candidate_labels_[i], nullptr, TRUE);
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (!was_visible) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (!IsWindowVisible(hwnd_)) return;
        RECT current{};
        const bool has_current = GetWindowRect(hwnd_, &current) != FALSE;
        if (!ime_visible_) {
            ime_visible_ = true;
            notify_ime_event(candidate_ime_show_event, hwnd_);
        } else if (was_visible && had_previous && has_current && !same_rect(previous, current)) {
            notify_ime_event(candidate_ime_change_event, hwnd_);
        }
    }

    static void notify_ime_event(DWORD event, HWND window) {
#ifdef BOSHIAMY_CANDIDATE_WINDOW_TESTING
        if (test_notify_sink_) test_notify_sink_(event, window, OBJID_CLIENT, CHILDID_SELF);
#endif
        NotifyWinEvent(event, window, OBJID_CLIENT, CHILDID_SELF);
    }

#ifdef BOSHIAMY_CANDIDATE_WINDOW_TESTING
    static inline TestNotifySink test_notify_sink_ = nullptr;
#endif

    static constexpr const wchar_t* class_name() { return L"BoshiamyCandidateWindow"; }

    static bool same_rect(const RECT& left, const RECT& right) {
        return left.left == right.left && left.top == right.top &&
               left.right == right.right && left.bottom == right.bottom;
    }

    static int scrollbar_width_for_dpi(UINT dpi) {
        using GetSystemMetricsForDpiFn = int (WINAPI*)(int, UINT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        FARPROC address = user32 ? GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr;
        GetSystemMetricsForDpiFn get_for_dpi = nullptr;
        static_assert(sizeof(get_for_dpi) == sizeof(address));
        std::memcpy(&get_for_dpi, &address, sizeof(get_for_dpi));
        if (get_for_dpi) return std::max(1, get_for_dpi(SM_CXVSCROLL, dpi));
        return std::max(1, MulDiv(GetSystemMetrics(SM_CXVSCROLL), static_cast<int>(dpi), 96));
    }

    static std::wstring make_scroll_signature(const std::wstring& composition,
                                               const std::wstring& page,
                                               const std::vector<std::wstring>& rows) {
        std::wstring key = composition + L"\x1f" + page;
        for (const auto& row : rows) {
            key += L"\x1e";
            key += std::to_wstring(row.size());
            key += L":";
            key += row;
        }
        return key;
    }

    static int scaled(int dip, int dpi) { return MulDiv(dip, dpi, 96); }

    struct PopupOrigin { int x; int top; };
    static PopupOrigin popup_origin(const RECT& anchor, UINT dpi, const RECT& workarea,
                                    int width, int height, bool candidate) {
        int x = anchor.left + (candidate ? scaled(5, static_cast<int>(dpi)) : 0);
        int top = anchor.bottom + (candidate ? scaled(8, static_cast<int>(dpi)) : 0);
        if (top + height > workarea.bottom) top = anchor.top - height;
        x = std::clamp(x, static_cast<int>(workarea.left), static_cast<int>(workarea.right) - width);
        top = std::clamp(top, static_cast<int>(workarea.top), static_cast<int>(workarea.bottom) - height);
        return {x, top};
    }

    static bool valid_anchor(const RECT& r) { return r.right >= r.left && r.bottom > r.top; }
    static bool valid_workarea(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

    static std::wstring utf8_to_wide(const std::string& value) {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0) return {};
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                 static_cast<int>(value.size()), result.data(), count)) return {};
        return result;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        CandidateWindow* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<CandidateWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_MOUSEWHEEL && self) {
            self->scroll_by_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }
        if (message == WM_VSCROLL && self) {
            self->handle_vertical_scroll(LOWORD(wparam));
            return 0;
        }
        if (message == WM_CTLCOLORSTATIC && self) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE && self) {
            self->hide();
            return 0;
        }
        if (message == WM_WINDOWPOSCHANGED && self) {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(lparam);
            if (position && (position->flags & SWP_HIDEWINDOW) &&
                !IsWindowVisible(window) && self->ime_visible_) {
                self->ime_visible_ = false;
                notify_ime_event(candidate_ime_hide_event, window);
            }
        }
        if (message == WM_ERASEBKGND) {
            RECT rect{};
            GetClientRect(window, &rect);
            HDC dc = reinterpret_cast<HDC>(wparam);
            FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));
            const bool official = self && self->normal_candidate_mode_;
            const COLORREF previous = SetDCBrushColor(dc, official ? RGB(41, 171, 226) : RGB(0, 120, 215));
            const int thickness = official ? std::max(1, scaled(3, self->font_dpi_ ? self->font_dpi_ : 96)) : 1;
            for (int i = 0; i < thickness; ++i) {
                RECT stroke{rect.left + i, rect.top + i, rect.right - i, rect.bottom - i};
                if (stroke.right > stroke.left && stroke.bottom > stroke.top)
                    FrameRect(dc, &stroke, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            }
            SetDCBrushColor(dc, previous);
            return 1;
        }
        if (message == WM_NCDESTROY) {
            if (self && self->hwnd_ == window) {
                if (self->ime_visible_) {
                    self->ime_visible_ = false;
                    notify_ime_event(candidate_ime_hide_event, window);
                }
                self->hwnd_ = nullptr;
                self->composition_label_ = nullptr;
                self->viewport_ = nullptr;
                self->viewport_proc_ = nullptr;
                self->candidate_labels_.fill(nullptr);
                self->candidate_label_procs_.fill(nullptr);
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK viewport_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        HWND root = GetAncestor(window, GA_ROOT);
        CandidateWindow* self = root
            ? reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(root, GWLP_USERDATA)) : nullptr;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_MOUSEWHEEL && self) {
            SendMessageW(root, WM_MOUSEWHEEL, wparam, lparam);
            return 0;
        }
        if (message == WM_CTLCOLORSTATIC && self) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            if (control == self->candidate_labels_[0] && IsWindowVisible(control)) {
                SetTextColor(dc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                SetBkColor(dc, GetSysColor(COLOR_HIGHLIGHT));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_HIGHLIGHT));
            }
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        if (self && self->viewport_proc_) {
            return CallWindowProcW(self->viewport_proc_, window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK candidate_label_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_MOUSEWHEEL) {
            HWND root = GetAncestor(window, GA_ROOT);
            if (root) SendMessageW(root, WM_MOUSEWHEEL, wparam, lparam);
            return 0;
        }
        HWND root = GetAncestor(window, GA_ROOT);
        CandidateWindow* self = root
            ? reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(root, GWLP_USERDATA)) : nullptr;
        if (self) {
            for (std::size_t i = 0; i < self->candidate_labels_.size(); ++i) {
                if (self->candidate_labels_[i] == window && self->candidate_label_procs_[i]) {
                    if (message == WM_PAINT) {
                        PAINTSTRUCT paint{};
                        HDC dc = BeginPaint(window, &paint);
                        if (dc) self->paint_candidate_label(window, dc, i);
                        EndPaint(window, &paint);
                        return 0;
                    }
                    if (message == WM_PRINTCLIENT || message == WM_PRINT) {
                        self->paint_candidate_label(window, reinterpret_cast<HDC>(wparam), i);
                        return 0;
                    }
                    if (message == WM_ERASEBKGND) return 1;
                    return CallWindowProcW(self->candidate_label_procs_[i], window, message, wparam, lparam);
                }
            }
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void paint_candidate_label(HWND window, HDC dc, std::size_t index) const {
        RECT bounds{};
        if (!dc || !GetClientRect(window, &bounds)) return;
        FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));

        const int length = GetWindowTextLengthW(window);
        if (length <= 0) return;
        std::wstring line(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(window, line.data(), length + 1);
        if (copied <= 0) return;
        line.resize(static_cast<std::size_t>(copied));
        const std::size_t separator = line.find(L' ');
        if (separator == std::wstring::npos) return;

        const std::wstring number = line.substr(0, separator);
        const std::wstring candidate = line.substr(separator + 1);
        HGDIOBJ previous_font = font_ ? SelectObject(dc, font_) : nullptr;
        const int candidate_left = normal_candidate_mode_
            ? std::max(1, scaled(34, font_dpi_ ? font_dpi_ : 96))
            : measure_inline_width(dc, line.substr(0, separator + 1));
        RECT candidate_rect{candidate_left, bounds.top, bounds.right, bounds.bottom};
        const bool selected = index == selected_row_;
        if (selected) {
            if (normal_candidate_mode_) {
                const COLORREF previous = SetDCBrushColor(dc, RGB(0, 109, 199));
                RECT selection = candidate_rect;
                selection.bottom = std::max(selection.top, selection.bottom -
                    scaled(2, font_dpi_ ? font_dpi_ : 96));
                FillRect(dc, &selection, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
                SetDCBrushColor(dc, previous);
            } else FillRect(dc, &candidate_rect, GetSysColorBrush(COLOR_HIGHLIGHT));
        }

        const int old_bk_mode = SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, normal_candidate_mode_ ? RGB(180, 180, 180) : GetSysColor(COLOR_WINDOWTEXT));
        RECT number_rect{bounds.left, bounds.top, candidate_left, bounds.bottom};
        if (normal_candidate_mode_) number_rect.left += scaled(7, font_dpi_ ? font_dpi_ : 96);
        if (normal_candidate_mode_) OffsetRect(&number_rect, 0, -scaled(2, font_dpi_ ? font_dpi_ : 96));
        DrawTextW(dc, number.c_str(), static_cast<int>(number.size()), &number_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, normal_candidate_mode_ ? (selected ? RGB(255,255,255) : RGB(0,0,0))
                                               : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        if (normal_candidate_mode_) OffsetRect(&candidate_rect, 0, -scaled(2, font_dpi_ ? font_dpi_ : 96));
        DrawTextW(dc, candidate.c_str(), static_cast<int>(candidate.size()), &candidate_rect,
                  normal_candidate_mode_ ? ((candidate_heights_[index] > scaled(24, font_dpi_ ? font_dpi_ : 96)
                      ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER) | DT_NOPREFIX)
                                         : DT_WORDBREAK | DT_NOPREFIX);
        SetBkMode(dc, old_bk_mode);
        if (previous_font) SelectObject(dc, previous_font);
    }

    static int measure_inline_width(HDC dc, const std::wstring& text) {
        SIZE size{};
        if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size)) return 0;
        return size.cx;
    }

    void layout_candidate_rows() {
        int row_top = -scroll_offset_;
        for (std::size_t i = 0; i < candidate_labels_.size(); ++i) {
            HWND label = candidate_labels_[i];
            if (i >= candidate_heights_.size()) {
                ShowWindow(label, SW_HIDE);
                continue;
            }
            const int row_height = candidate_heights_[i];
            const int visible_top = std::max(0, row_top);
            const int visible_bottom = std::min(candidate_viewport_height_, row_top + row_height);
            if (visible_bottom <= visible_top || candidate_inner_width_ <= 0) {
                ShowWindow(label, SW_HIDE);
                row_top += row_height;
                continue;
            }
            MoveWindow(label, 0, row_top, candidate_inner_width_, row_height, TRUE);
            ShowWindow(label, SW_SHOWNOACTIVATE);
            row_top += row_height;
        }
    }

    void set_scroll_offset(int requested) {
        const int next = std::clamp(requested, 0, candidate_scroll_max_);
        if (next == scroll_offset_) return;
        scroll_offset_ = next;
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_POS;
        info.nPos = scroll_offset_;
        SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
        layout_candidate_rows();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void scroll_by_wheel(int delta) {
        if (candidate_scroll_max_ <= 0) return;
        wheel_delta_remainder_ += delta;
        const int notches = wheel_delta_remainder_ / WHEEL_DELTA;
        if (notches == 0) return;
        wheel_delta_remainder_ -= notches * WHEEL_DELTA;
        const int distance = scaled(48, font_dpi_ ? font_dpi_ : 96);
        set_scroll_offset(scroll_offset_ - notches * distance);
    }

    void handle_vertical_scroll(WORD request) {
        if (candidate_scroll_max_ <= 0) return;
        int next = scroll_offset_;
        const int line = std::max(1, scaled(20, font_dpi_ ? font_dpi_ : 96));
        switch (request) {
        case SB_LINEUP: next -= line; break;
        case SB_LINEDOWN: next += line; break;
        case SB_PAGEUP: next -= candidate_viewport_height_; break;
        case SB_PAGEDOWN: next += candidate_viewport_height_; break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = candidate_scroll_max_; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd_, SB_VERT, &info)) next = info.nTrackPos;
            break;
        }
        default: return;
        }
        set_scroll_offset(next);
    }

    struct RegistrationState {
        std::mutex mutex;
        HINSTANCE instance = nullptr;
        std::size_t references = 0;
    };

    static RegistrationState& registration_state() {
        static RegistrationState state;
        return state;
    }

    static bool acquire_class(HINSTANCE& instance) {
        RegistrationState& state = registration_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.references != 0) {
            ++state.references;
            instance = state.instance;
            return true;
        }

        HMODULE module = nullptr;
        LPCWSTR address = reinterpret_cast<LPCWSTR>(&window_proc);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                address, &module)) return false;

        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = window_proc;
        cls.hInstance = module;
        cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        cls.lpszClassName = class_name();
        if (!RegisterClassExW(&cls)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
            WNDCLASSEXW existing{};
            existing.cbSize = sizeof(existing);
            if (!GetClassInfoExW(module, class_name(), &existing) ||
                existing.lpfnWndProc != window_proc || existing.hInstance != module) return false;
        }
        state.instance = module;
        state.references = 1;
        instance = module;
        return true;
    }

    void release_class() {
        if (!class_ref_) return;
        RegistrationState& state = registration_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        class_ref_ = false;
        if (state.references == 0) return;
        --state.references;
        if (state.references == 0) {
            UnregisterClassW(class_name(), state.instance);
            state.instance = nullptr;
        }
    }

    void update_font(int dpi, bool official_candidates) {
        if (font_ && font_dpi_ == dpi && font_official_candidates_ == official_candidates) return;
        HFONT next=CreateFontW(-MulDiv(official_candidates ? 14 : 10,dpi,72),0,0,0,
            official_candidates ? FW_MEDIUM : FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,
            official_candidates ? L"Microsoft JhengHei" : L"Microsoft JhengHei UI");
        if(!next)return;
        SendMessageW(composition_label_,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
        for(HWND label:candidate_labels_)SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
        HFONT previous=font_;font_=next;font_dpi_=dpi;font_official_candidates_=official_candidates;
        if(previous)DeleteObject(previous);
    }

    int measure_text_width(const std::wstring& text) const {
        HDC dc = GetDC(hwnd_);
        if (!dc) return 0;
        HGDIOBJ previous = font_ ? SelectObject(dc, font_) : nullptr;
        SIZE size{};
        const bool measured = GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size) != FALSE;
        if (previous) SelectObject(dc, previous);
        ReleaseDC(hwnd_, dc);
        return measured ? size.cx : 0;
    }

    int measure_text_height(const std::wstring& text, int width) const {
        HDC dc = GetDC(hwnd_);
        if (!dc) return scaled(18, font_dpi_ ? font_dpi_ : 96);
        HGDIOBJ previous = font_ ? SelectObject(dc, font_) : nullptr;
        RECT measured{0, 0, std::max(1, width), 0};
        const int result = DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured,
                                     DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        TEXTMETRICW metrics{};
        const bool got_metrics = GetTextMetricsW(dc, &metrics) != FALSE;
        if (previous) SelectObject(dc, previous);
        ReleaseDC(hwnd_, dc);
        if (result == 0) return got_metrics ? metrics.tmHeight : 18;
        return std::max(static_cast<int>(measured.bottom - measured.top), got_metrics ? static_cast<int>(metrics.tmHeight) : 18);
    }

    int measure_candidate_height(const std::wstring& line, int width) const {
        const std::size_t separator = line.find(L' ');
        if (separator == std::wstring::npos) return measure_text_height(line, width);
        const std::wstring index = line.substr(0, separator);
        const std::wstring candidate = line.substr(separator + 1);
        const int text_left = measure_text_width(line.substr(0, separator + 1));
        const int candidate_width = std::max(1, width - text_left);
        return std::max(measure_text_height(index, width),
                        measure_text_height(candidate, candidate_width));
    }

    HWND create_label(HWND parent) const {
        if (!parent) return nullptr;
        return CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               0, 0, 0, 0, parent, nullptr, module_instance_, nullptr);
    }

    HWND owner_ = nullptr;
    HINSTANCE module_instance_ = nullptr;
    bool class_ref_ = false;
    bool ime_visible_ = false;
    HWND hwnd_ = nullptr;
    HWND composition_label_ = nullptr;
    HWND viewport_ = nullptr;
    WNDPROC viewport_proc_ = nullptr;
    std::array<HWND, 10> candidate_labels_{};
    std::array<WNDPROC, 10> candidate_label_procs_{};
    HFONT font_ = nullptr;
    int font_dpi_ = 0;
    bool font_official_candidates_ = false;
    bool normal_candidate_mode_ = false;
    std::size_t selected_row_ = 0;
    std::wstring scroll_signature_;
    std::vector<int> candidate_heights_;
    int candidate_inner_width_ = 0;
    int candidate_x_ = 0;
    int candidate_viewport_top_ = 0;
    int candidate_viewport_bottom_ = 0;
    int candidate_viewport_height_ = 0;
    int candidate_total_height_ = 0;
    int candidate_scroll_max_ = 0;
    int scroll_offset_ = 0;
    int wheel_delta_remainder_ = 0;
};

} // namespace boshiamy
#endif
