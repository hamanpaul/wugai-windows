#include "../tsf/language_bar.hpp"
#include "../tsf/mode_icon.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace boshiamy;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while (0)

namespace {
// TF_LBI_STYLE_SHOWNINTRAY from the TSF language bar item style contract.
constexpr DWORD kTfLbiStyleShownInTray = 0x00000002;

class IconSink final : public ITfLangBarItemSink {
public:
    ULONG refs = 1;
    std::vector<DWORD> updates;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfLangBarItemSink)) {
            *out = static_cast<ITfLangBarItemSink *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnUpdate(DWORD flags) override {
        updates.push_back(flags);
        return S_OK;
    }
};

struct IconPixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> bgra;
    std::vector<std::uint8_t> mask;
};

IconPixels pixels(HICON icon) {
    IconPixels out;
    ICONINFO info{};
    CHECK(icon && GetIconInfo(icon, &info));
    BITMAP color{};
    CHECK(info.hbmColor && GetObjectW(info.hbmColor, sizeof(color), &color) == sizeof(color));
    out.width = color.bmWidth;
    out.height = color.bmHeight;
    BITMAPINFO color_info{};
    color_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    color_info.bmiHeader.biWidth = out.width;
    color_info.bmiHeader.biHeight = -out.height;
    color_info.bmiHeader.biPlanes = 1;
    color_info.bmiHeader.biBitCount = 32;
    color_info.bmiHeader.biCompression = BI_RGB;
    out.bgra.resize(static_cast<std::size_t>(out.width * out.height));
    HDC dc = GetDC(nullptr);
    CHECK(dc && GetDIBits(dc, info.hbmColor, 0, out.height, out.bgra.data(),
                           &color_info, DIB_RGB_COLORS) == out.height);

    BITMAP mask{};
    CHECK(info.hbmMask && GetObjectW(info.hbmMask, sizeof(mask), &mask) == sizeof(mask));
    struct MaskInfo { BITMAPINFOHEADER header; RGBQUAD colors[2]; } mask_info{};
    mask_info.header.biSize = sizeof(BITMAPINFOHEADER);
    mask_info.header.biWidth = out.width;
    mask_info.header.biHeight = -out.height;
    mask_info.header.biPlanes = 1;
    mask_info.header.biBitCount = 1;
    mask_info.header.biCompression = BI_RGB;
    mask_info.colors[0] = {0, 0, 0, 0};
    mask_info.colors[1] = {255, 255, 255, 0};
    const std::size_t stride = ((static_cast<std::size_t>(out.width) + 31) / 32) * 4;
    std::vector<std::uint8_t> raw(stride * static_cast<std::size_t>(out.height));
    CHECK(GetDIBits(dc, info.hbmMask, 0, out.height, raw.data(),
        reinterpret_cast<BITMAPINFO *>(&mask_info), DIB_RGB_COLORS) == out.height);
    ReleaseDC(nullptr, dc);
    out.mask.resize(static_cast<std::size_t>(out.width * out.height));
    for (int y = 0; y < out.height; ++y) for (int x = 0; x < out.width; ++x) {
        const auto byte = raw[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x / 8)];
        out.mask[static_cast<std::size_t>(y * out.width + x)] =
            static_cast<std::uint8_t>((byte >> (7 - (x & 7))) & 1);
    }
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    return out;
}

HICON current_icon(ITfLangBarItemButton *button) {
    HICON icon = nullptr;
    CHECK(button->GetIcon(&icon) == S_OK && icon);
    return icon;
}

void check_glyph_pixels(const IconPixels& icon) {
    CHECK(icon.width >= 8 && icon.height >= 8);
    unsigned visible = 0, bright = 0, transparent = 0;
    for (std::size_t i = 0; i < icon.bgra.size(); ++i) {
        const std::uint32_t pixel = icon.bgra[i];
        const unsigned blue = pixel & 0xff, green = (pixel >> 8) & 0xff;
        const unsigned red = (pixel >> 16) & 0xff, alpha = pixel >> 24;
        CHECK(red == green && green == blue);
        CHECK(red <= alpha);
        CHECK(icon.mask[i] == (alpha == 0));
        visible += alpha != 0;
        bright += alpha >= 192 && red >= 192;
        transparent += alpha == 0;
    }
    CHECK(visible > 10 && bright > 5 && transparent > 10);
}

void verify_mode_switch(IconSource source) {
    const CLSID clsid = {0x5d7c4291, 0x2b34, 0x4ce8, {0x8a,0x11,0x39,0x62,0x71,0x05,0xc4,0x2e}};
    LanguageBarItem *item = nullptr;
    std::vector<MenuCommand> clicks;
    item = new LanguageBarItem(clsid, [&](MenuCommand command) {
        clicks.push_back(command);
        return item->update_display(LanguageBarMode::Traditional,
            command == MenuCommand::English, false);
    }, std::move(source));
    ITfLangBarItemButton *button = nullptr;
    CHECK(item->QueryInterface(language_bar_detail::kIidLangBarItemButton,
                                reinterpret_cast<void **>(&button)) == S_OK);
    ITfSource *sink_source = nullptr;
    CHECK(item->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&sink_source)) == S_OK);
    IconSink sink;
    DWORD cookie = 0;
    CHECK(sink_source->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie) == S_OK);

    TF_LANGBARITEMINFO info{};
    CHECK(button->GetInfo(&info) == S_OK);
    CHECK((info.dwStyle & 0x00010000) != 0); // SDK TF_LBI_STYLE_BTN_BUTTON.
    CHECK((info.dwStyle & kTfLbiStyleShownInTray) != 0);
    BSTR text = nullptr;
    CHECK(button->GetText(&text) == S_OK && std::wstring(text) == L"中");
    SysFreeString(text);
    CHECK(button->OnClick(TF_LBI_CLK_LEFT, POINT{}, nullptr) == S_OK);
    CHECK(clicks.size() == 1 && clicks.back() == MenuCommand::English);
    CHECK(button->GetText(&text) == S_OK && std::wstring(text) == L"英");
    SysFreeString(text);
    CHECK(button->OnClick(TF_LBI_CLK_LEFT, POINT{}, nullptr) == S_OK);
    CHECK(clicks.size() == 2 && clicks.back() == MenuCommand::Chinese);
    CHECK(button->GetText(&text) == S_OK && std::wstring(text) == L"中");
    SysFreeString(text);
    CHECK(sink.updates.size() == 2);
    for (DWORD flags : sink.updates)
        CHECK((flags & (TF_LBI_STATUS | TF_LBI_ICON | TF_LBI_TEXT)) ==
              (TF_LBI_STATUS | TF_LBI_ICON | TF_LBI_TEXT));
    sink.updates.clear();

    CHECK(item->update_display(LanguageBarMode::Traditional, false, false) == S_FALSE);
    HICON chinese = current_icon(button);
    const IconPixels chinese_pixels = pixels(chinese);
    HICON expected_chinese = mode_icon::create(mode_icon::State::Chinese,
        chinese_pixels.width, chinese_pixels.height);
    CHECK(expected_chinese);
    const IconPixels expected_chinese_pixels = pixels(expected_chinese);
    CHECK(chinese_pixels.bgra == expected_chinese_pixels.bgra &&
          chinese_pixels.mask == expected_chinese_pixels.mask);
    check_glyph_pixels(chinese_pixels);

    CHECK(item->update_display(LanguageBarMode::Traditional, true, false) == S_OK);
    HICON english = current_icon(button);
    const IconPixels english_pixels = pixels(english);
    HICON expected_english = mode_icon::create(mode_icon::State::English,
        english_pixels.width, english_pixels.height);
    CHECK(expected_english);
    const IconPixels expected_english_pixels = pixels(expected_english);
    CHECK(english_pixels.bgra == expected_english_pixels.bgra &&
          english_pixels.mask == expected_english_pixels.mask);
    CHECK(english_pixels.bgra != chinese_pixels.bgra);
    check_glyph_pixels(english_pixels);

    CHECK(item->update_display(LanguageBarMode::Fast, true, false) == S_OK);
    HICON fast_english = current_icon(button);
    CHECK(pixels(fast_english).bgra == english_pixels.bgra);

    CHECK(item->update_display(LanguageBarMode::Traditional, false, false) == S_OK);
    HICON chinese_again = current_icon(button);
    const IconPixels chinese_again_pixels = pixels(chinese_again);
    CHECK(chinese_again_pixels.bgra == chinese_pixels.bgra &&
          chinese_again_pixels.mask == chinese_pixels.mask);
    CHECK(sink.updates.size() == 3);
    for (DWORD flags : sink.updates)
        CHECK((flags & (TF_LBI_STATUS | TF_LBI_ICON)) == (TF_LBI_STATUS | TF_LBI_ICON));

    CHECK(DestroyIcon(chinese));
    CHECK(DestroyIcon(english));
    CHECK(DestroyIcon(fast_english));
    CHECK(DestroyIcon(chinese_again));
    CHECK(DestroyIcon(expected_chinese));
    CHECK(DestroyIcon(expected_english));
    CHECK(sink_source->UnadviseSink(cookie) == S_OK && sink.refs == 1);
    sink_source->Release();
    button->Release();
    item->Release();
}
} // namespace

int main(int argc, char **argv) try {
    const std::wstring path = argc > 1 ? std::filesystem::u8path(argv[1]).wstring()
                                       : L"assets/liukai.ico";
    const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);
    CHECK(width >= 8 && height >= 8);
    verify_mode_switch(IconSource::from_path(path));
    verify_mode_switch(IconSource::from_resource(GetModuleHandleW(nullptr), 101));
    verify_mode_switch(IconSource::from_path(L"this-file-does-not-exist-language-bar-test.ico"));
    std::cout << "language bar MODE glyph switch passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}

