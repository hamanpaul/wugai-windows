#ifndef BOSHIAMY_TSF_LANGUAGE_BAR_HPP
#define BOSHIAMY_TSF_LANGUAGE_BAR_HPP

#include <windows.h>
#include <oleauto.h>
#include <msctf.h>
#include <ctfutb.h>

#include "../engine.hpp"
#include "../keyboard_modes.hpp"
#include "mode_icon.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// MinGW-w64's ctfutb.h currently omits these SDK interfaces. Keep the ABI
// declarations guarded so Windows SDK builds use their own declarations.
#ifndef __ITfMenu_INTERFACE_DEFINED__
#define __ITfMenu_INTERFACE_DEFINED__
MIDL_INTERFACE("6f8a98e4-aaa0-4f15-8c5b-07e0df0a3dd8")
ITfMenu : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AddMenuItem(UINT uId, DWORD dwFlags,
        HBITMAP hbmp, HBITMAP hbmpMask, const WCHAR *pch, ULONG cch,
        ITfMenu **ppMenu) = 0;
};
#endif

#ifdef __MINGW32__
enum TfLBIClick { TF_LBI_CLK_RIGHT = 1, TF_LBI_CLK_LEFT = 2 };
#endif

#ifndef __ITfLangBarItemButton_INTERFACE_DEFINED__
#define __ITfLangBarItemButton_INTERFACE_DEFINED__
MIDL_INTERFACE("28c7f1d0-de25-11d2-afdd-00105a2799b5")
ITfLangBarItemButton : public ITfLangBarItem {
    virtual HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT pt, const RECT *prcArea) = 0;
    virtual HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu *pMenu) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT wID) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetIcon(HICON *phIcon) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetText(BSTR *pbstrText) = 0;
};
#endif

#ifndef TF_LBI_ICON
#define TF_LBI_ICON 0x00000001
#endif
#ifndef TF_LBI_TEXT
#define TF_LBI_TEXT 0x00000002
#endif
#ifndef TF_LBI_TOOLTIP
#define TF_LBI_TOOLTIP 0x00000004
#endif
#ifndef TF_LBI_STATUS
#define TF_LBI_STATUS 0x00010000
#endif
#ifndef TF_LBI_BTNALL
#define TF_LBI_BTNALL (TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP)
#endif
#ifndef TF_LBI_STYLE_HIDDENSTATUSCONTROL
#define TF_LBI_STYLE_HIDDENSTATUSCONTROL 0x00000001
#endif
#ifndef TF_LBI_STYLE_SHOWNINTRAY
#define TF_LBI_STYLE_SHOWNINTRAY 0x00000002
#endif
#ifndef TF_LBI_STYLE_BTN_MENU
#define TF_LBI_STYLE_BTN_MENU 0x00020000
#endif
#ifndef TF_LBI_STYLE_BTN_BUTTON
#define TF_LBI_STYLE_BTN_BUTTON 0x00010000
#endif
#ifndef TF_LBI_STATUS_HIDDEN
#define TF_LBI_STATUS_HIDDEN 0x00000001
#endif
#ifndef TF_LBMENUF_CHECKED
#define TF_LBMENUF_CHECKED 0x00000001
#endif
#ifndef TF_LBMENUF_RADIOCHECKED
#define TF_LBMENUF_RADIOCHECKED 0x00000008
#endif
namespace boshiamy {

using LanguageBarMode = Mode;
enum class MenuCommand : UINT {
    Traditional = 0x5101, Simplified, TradToSimplified, Japanese, Fast,
    English, Chinese, Fullwidth, Halfwidth, Lookup, Settings = 0x5120
};

struct LanguageBarDisplay {
    LanguageBarMode mode = LanguageBarMode::Traditional;
    bool english = false;
    bool fullwidth = false;
};

struct IconSource {
    HINSTANCE module = nullptr;
    WORD resource_id = 0;
    std::wstring path;

    static IconSource from_resource(HINSTANCE module, WORD id) {
        return {module, id, {}};
    }
    static IconSource from_path(std::wstring path) {
        return {nullptr, 0, std::move(path)};
    }
};

namespace language_bar_detail {
inline const GUID kIidLangBarItemButton = {0x28c7f1d0, 0xde25, 0x11d2, {0xaf, 0xdd, 0x00, 0x10, 0x5a, 0x27, 0x99, 0xb5}};
inline const GUID kGuidLbiInputMode =
    {0x2c77a81e, 0x41cc, 0x4178, {0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6}};
inline constexpr wchar_t kBrandName[] = L"嘸．改";
inline constexpr HRESULT kConnectCannotConnect = static_cast<HRESULT>(0x80040202L);
inline constexpr HRESULT kConnectNoConnection = static_cast<HRESULT>(0x80040200L);

inline const wchar_t *mode_name(LanguageBarMode mode) noexcept {
    switch (mode) {
    case LanguageBarMode::Simplified: return L"簡體 / Simplified";
    case LanguageBarMode::TradToSimplified: return L"打繁出簡 / Traditional input, Simplified output";
    case LanguageBarMode::Japanese: return L"日文 / Japanese";
    case LanguageBarMode::Fast: return L"快打 / Fast";
    case LanguageBarMode::Traditional: default: return L"繁體 / Traditional";
    }
}
inline UINT command_id(MenuCommand command) noexcept {
    return static_cast<UINT>(command);
}

inline HICON load_icon_source(const IconSource& source) noexcept {
    int width = GetSystemMetrics(SM_CXSMICON);
    int height = GetSystemMetrics(SM_CYSMICON);
    if (width <= 0) width = 16;
    if (height <= 0) height = 16;
    if (source.module && source.resource_id) {
        SetLastError(ERROR_SUCCESS);
        return static_cast<HICON>(LoadImageW(source.module,
            MAKEINTRESOURCEW(source.resource_id), IMAGE_ICON, width, height, 0));
    }
    if (!source.path.empty()) {
        SetLastError(ERROR_SUCCESS);
        return static_cast<HICON>(LoadImageW(nullptr, source.path.c_str(), IMAGE_ICON,
            width, height, LR_LOADFROMFILE));
    }
    return nullptr;
}

inline HICON make_grayscale_icon(HICON source) noexcept {
    if (!source) return nullptr;
    ICONINFO source_info{};
    if (!GetIconInfo(source, &source_info)) return nullptr;
    HICON result = nullptr;
    HDC dc = nullptr;
    HBITMAP color_bitmap = nullptr;
    HBITMAP mask_bitmap = nullptr;
    void* color_bits = nullptr;
    void* mask_bits = nullptr;
    do {
        BITMAP dimensions{};
        if (!source_info.hbmColor || !source_info.hbmMask ||
            GetObjectW(source_info.hbmColor, sizeof(dimensions), &dimensions) != sizeof(dimensions) ||
            dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0) break;
        const int width = dimensions.bmWidth, height = dimensions.bmHeight;
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixel_count > (256u * 256u)) break;
        std::vector<BYTE> bgra;
        const size_t mask_stride = ((static_cast<size_t>(width) + 31u) / 32u) * 4u;
        std::vector<BYTE> mask_bytes;
        try {
            bgra.resize(pixel_count * 4u);
            mask_bytes.resize(mask_stride * static_cast<size_t>(height));
        } catch (...) { break; }
        dc = GetDC(nullptr);
        if (!dc) break;
        BITMAPINFO color_info{};
        color_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        color_info.bmiHeader.biWidth = width;
        color_info.bmiHeader.biHeight = -height;
        color_info.bmiHeader.biPlanes = 1;
        color_info.bmiHeader.biBitCount = 32;
        color_info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(dc, source_info.hbmColor, 0, static_cast<UINT>(height),
                bgra.data(), &color_info, DIB_RGB_COLORS) != height) break;
        struct MaskInfo { BITMAPINFOHEADER header; RGBQUAD colors[2]; } mask_info{};
        mask_info.header.biSize = sizeof(BITMAPINFOHEADER);
        mask_info.header.biWidth = width;
        mask_info.header.biHeight = -height;
        mask_info.header.biPlanes = 1;
        mask_info.header.biBitCount = 1;
        mask_info.header.biCompression = BI_RGB;
        mask_info.colors[0] = {0, 0, 0, 0};
        mask_info.colors[1] = {255, 255, 255, 0};
        if (GetDIBits(dc, source_info.hbmMask, 0, static_cast<UINT>(height),
                mask_bytes.data(), reinterpret_cast<BITMAPINFO *>(&mask_info),
                DIB_RGB_COLORS) != height) break;

        for (size_t offset = 0; offset < bgra.size(); offset += 4u) {
            const unsigned blue = bgra[offset];
            const unsigned green = bgra[offset + 1];
            const unsigned red = bgra[offset + 2];
            const BYTE gray = static_cast<BYTE>((299u * red + 587u * green +
                114u * blue + 500u) / 1000u);
            bgra[offset] = gray;
            bgra[offset + 1] = gray;
            bgra[offset + 2] = gray;
        }

        BITMAPINFO out_color_info{};
        out_color_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        out_color_info.bmiHeader.biWidth = width;
        out_color_info.bmiHeader.biHeight = -height;
        out_color_info.bmiHeader.biPlanes = 1;
        out_color_info.bmiHeader.biBitCount = 32;
        out_color_info.bmiHeader.biCompression = BI_RGB;
        color_bitmap = CreateDIBSection(dc, &out_color_info, DIB_RGB_COLORS,
            &color_bits, nullptr, 0);
        if (!color_bitmap || !color_bits) break;
        std::memcpy(color_bits, bgra.data(), bgra.size());
        mask_bitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&mask_info),
            DIB_RGB_COLORS, &mask_bits, nullptr, 0);
        if (!mask_bitmap || !mask_bits) break;
        std::memcpy(mask_bits, mask_bytes.data(), mask_bytes.size());
        ICONINFO out_info{};
        out_info.fIcon = TRUE;
        out_info.hbmColor = color_bitmap;
        out_info.hbmMask = mask_bitmap;
        result = CreateIconIndirect(&out_info);
    } while (false);
    if (mask_bitmap) DeleteObject(mask_bitmap);
    if (color_bitmap) DeleteObject(color_bitmap);
    if (dc) ReleaseDC(nullptr, dc);
    if (source_info.hbmMask) DeleteObject(source_info.hbmMask);
    if (source_info.hbmColor) DeleteObject(source_info.hbmColor);
    return result;
}
} // namespace language_bar_detail

static_assert(std::is_base_of<ITfLangBarItem, ITfLangBarItemButton>::value, "ITfLangBarItemButton must preserve the ITfLangBarItem vtable");

class LanguageBarItem final : public ITfLangBarItemButton, public ITfSource {
public:
    using MenuAction = std::function<HRESULT(MenuCommand)>;

    explicit LanguageBarItem(const CLSID &service_clsid, MenuAction action,
        IconSource icon = IconSource::from_path(L"assets/liukai.ico"), std::shared_ptr<void> module_lifetime = {})
        : module_lifetime_(std::move(module_lifetime)), service_clsid_(service_clsid), action_(std::move(action)), icon_(std::move(icon)) {}

    ~LanguageBarItem() {
        for (auto &entry : sinks_) entry.sink->Release();
    }

    LanguageBarItem(const LanguageBarItem &) = delete;
    LanguageBarItem &operator=(const LanguageBarItem &) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfLangBarItem))
            *out = static_cast<ITfLangBarItem *>(static_cast<ITfLangBarItemButton *>(this));
        else if (IsEqualIID(iid, language_bar_detail::kIidLangBarItemButton))
            *out = static_cast<ITfLangBarItemButton *>(this);
        else if (IsEqualIID(iid, IID_ITfSource))
            *out = static_cast<ITfSource *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --refs_;
        if (!count) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO *info) override {
        if (!info) return E_INVALIDARG;
        ZeroMemory(info, sizeof(*info));
        info->clsidService = service_clsid_;
        info->guidItem = language_bar_detail::kGuidLbiInputMode;
        info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU | TF_LBI_STYLE_SHOWNINTRAY;
        lstrcpynW(info->szDescription, language_bar_detail::kBrandName, TF_LBI_DESC_MAXLEN);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD *status) override {
        if (!status) return E_INVALIDARG;
        *status = hidden_ ? TF_LBI_STATUS_HIDDEN : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override {
        const bool next = show == FALSE;
        if (hidden_ == next) return S_OK;
        hidden_ = next;
        notify(TF_LBI_STATUS);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR *tooltip) override {
        if (!tooltip) return E_INVALIDARG;
        const std::wstring value = tooltip_text();
        *tooltip = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return *tooltip ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click, POINT, const RECT *) override {
        if (click != TF_LBI_CLK_LEFT) return S_OK;
        if (!action_) return E_UNEXPECTED;
        try { return action_(display_.english ? MenuCommand::Chinese : MenuCommand::English); }
        catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu *menu) override {
        if (!menu) return E_INVALIDARG;
        struct Item { MenuCommand command; const wchar_t *text; };
        static constexpr Item items[] = {
            {MenuCommand::Traditional, L"繁體 / Traditional"},
            {MenuCommand::Simplified, L"簡體 / Simplified"},
            {MenuCommand::TradToSimplified, L"打繁出簡 / Traditional to Simplified"},
            {MenuCommand::Japanese, L"日文 / Japanese"},
            {MenuCommand::Fast, L"快打 / Fast"},
            {MenuCommand::English, L"English input"},
            {MenuCommand::Chinese, L"中文輸入 / Chinese input"},
            {MenuCommand::Fullwidth, L"全形 / Full-width"},
            {MenuCommand::Halfwidth, L"半形 / Half-width"},
            {MenuCommand::Lookup, L"查碼程式"},
            {MenuCommand::Settings, L"設定"},
        };
        for (const auto &item : items) {
            DWORD flags = 0;
            if (is_checked(item.command)) {
                flags = is_language_mode(item.command)
                    ? TF_LBMENUF_RADIOCHECKED : TF_LBMENUF_CHECKED;
            }
            HRESULT hr = menu->AddMenuItem(language_bar_detail::command_id(item.command), flags,
                nullptr, nullptr, item.text, static_cast<ULONG>(lstrlenW(item.text)), nullptr);
            if (FAILED(hr)) return hr;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT id) override {
        const auto command = command_from_id(id);
        if (!command) return E_INVALIDARG;
        if (!action_) return E_UNEXPECTED;
        try { return action_(*command); } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE GetIcon(HICON *icon) override {
        if (!icon) return E_INVALIDARG;
        *icon = nullptr;
        *icon = mode_icon::create(display_.english
            ? mode_icon::State::English : mode_icon::State::Chinese);
        return *icon ? S_OK : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetText(BSTR *text) override {
        if (!text) return E_INVALIDARG;
        const std::wstring label = mode_icon::glyph(display_.english
            ? mode_icon::State::English : mode_icon::State::Chinese);
        *text = SysAllocStringLen(label.data(), static_cast<UINT>(label.size()));
        return *text ? S_OK : E_OUTOFMEMORY;
    }

    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown *unknown, DWORD *cookie) override {
        if (!cookie) return E_INVALIDARG;
        *cookie = 0;
        if (!unknown || !IsEqualIID(iid, IID_ITfLangBarItemSink)) return language_bar_detail::kConnectCannotConnect;
        ITfLangBarItemSink *sink = nullptr;
        HRESULT hr = unknown->QueryInterface(iid, reinterpret_cast<void **>(&sink));
        if (FAILED(hr)) return language_bar_detail::kConnectCannotConnect;
        try {
            const DWORD next = next_cookie_++;
            sinks_.push_back({next, sink});
            *cookie = next;
            return S_OK;
        } catch (...) {
            sink->Release();
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie) override {
        for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
            if (it->cookie == cookie) {
                it->sink->Release();
                sinks_.erase(it);
                return S_OK;
            }
        }
        return language_bar_detail::kConnectNoConnection;
    }

    HRESULT update_display(Mode mode, const KeyboardModes &keyboard_modes) {
        return update_display(mode, keyboard_modes.english(), keyboard_modes.fullwidth());
    }

    HRESULT update_display(LanguageBarMode mode, bool english, bool fullwidth) {
        if (display_.mode == mode && display_.english == english && display_.fullwidth == fullwidth) return S_FALSE;
        display_ = {mode, english, fullwidth};
        notify(TF_LBI_BTNALL | TF_LBI_STATUS);
        return S_OK;
    }
    const LanguageBarDisplay &display_state() const noexcept { return display_; }

private:
    // Declared first so the module guard is destroyed after callback and sink state.
    std::shared_ptr<void> module_lifetime_;
    struct SinkEntry { DWORD cookie; ITfLangBarItemSink *sink; };
    std::atomic<ULONG> refs_{1};
    CLSID service_clsid_{};
    MenuAction action_;
    IconSource icon_;
    LanguageBarDisplay display_{};
    std::vector<SinkEntry> sinks_;
    DWORD next_cookie_ = 1;
    bool hidden_ = false;

    std::wstring tooltip_text() const {
        return std::wstring(display_.english ? L"English input" : L"中文輸入 / Chinese input") +
            L" · " + language_bar_detail::mode_name(display_.mode) +
            (display_.fullwidth ? L" · 全形 / Full-width" : L" · 半形 / Half-width");
    }
    static bool is_language_mode(MenuCommand command) noexcept {
        return command >= MenuCommand::Traditional && command <= MenuCommand::Fast;
    }
    bool is_checked(MenuCommand command) const noexcept {
        switch (command) {
        case MenuCommand::Traditional: return display_.mode == LanguageBarMode::Traditional;
        case MenuCommand::Simplified: return display_.mode == LanguageBarMode::Simplified;
        case MenuCommand::TradToSimplified: return display_.mode == LanguageBarMode::TradToSimplified;
        case MenuCommand::Japanese: return display_.mode == LanguageBarMode::Japanese;
        case MenuCommand::Fast: return display_.mode == LanguageBarMode::Fast;
        case MenuCommand::English: return display_.english;
        case MenuCommand::Chinese: return !display_.english;
        case MenuCommand::Fullwidth: return display_.fullwidth;
        case MenuCommand::Halfwidth: return !display_.fullwidth;
        case MenuCommand::Lookup: case MenuCommand::Settings: return false;
        }
        return false;
    }
    static bool is_mode_command(MenuCommand command) noexcept {
        return command >= MenuCommand::Traditional && command <= MenuCommand::Fast;
    }
    static std::optional<MenuCommand> command_from_id(UINT id) noexcept {
        if (id >= language_bar_detail::command_id(MenuCommand::Traditional) &&
            id <= language_bar_detail::command_id(MenuCommand::Lookup))
            return static_cast<MenuCommand>(id);
        if (id == language_bar_detail::command_id(MenuCommand::Settings)) return MenuCommand::Settings;
        return std::nullopt;
    }
    void notify(DWORD flags) noexcept {
        // Hold references across callbacks because a sink may unadvise itself.
        std::vector<ITfLangBarItemSink *> snapshot;
        try {
            snapshot.reserve(sinks_.size());
            for (auto &entry : sinks_) { entry.sink->AddRef(); snapshot.push_back(entry.sink); }
        } catch (...) {
            for (auto *sink : snapshot) sink->Release();
            return;
        }
        for (auto *sink : snapshot) { sink->OnUpdate(flags); sink->Release(); }
    }
};

inline HRESULT add_language_bar_item(ITfLangBarItemMgr *manager, LanguageBarItem *item) {
    if (!manager || !item) return E_INVALIDARG;
    return manager->AddItem(static_cast<ITfLangBarItem *>(item));
}

} // namespace boshiamy

#endif
