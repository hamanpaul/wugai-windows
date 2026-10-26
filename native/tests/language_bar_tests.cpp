#include "../tsf/language_bar.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
#include <filesystem>

using boshiamy::LanguageBarItem;
using boshiamy::LanguageBarMode;
using boshiamy::MenuCommand;
using boshiamy::IconSource;
using namespace boshiamy::language_bar_detail;

static_assert(std::is_base_of<ITfLangBarItem, ITfLangBarItemButton>::value, "button inherits the complete lang-bar item vtable");

namespace {
class FakeMenu final : public ITfMenu {
public:
    struct Entry { UINT id; DWORD flags; std::wstring text; };
    ULONG refs = 1;
    std::vector<Entry> entries;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown)) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE AddMenuItem(UINT id, DWORD flags, HBITMAP, HBITMAP,
        const WCHAR *text, ULONG count, ITfMenu **submenu) override {
        assert(submenu == nullptr);
        entries.push_back({id, flags, std::wstring(text, count)});
        return S_OK;
    }
};

class FakeSink final : public ITfLangBarItemSink {
public:
    ULONG refs = 1;
    std::vector<DWORD> updates;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ITfLangBarItemSink)) {
            *out = static_cast<ITfLangBarItemSink*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnUpdate(DWORD flags) override { updates.push_back(flags); return S_OK; }
};
}

int main(int argc, char** argv) {
    const std::wstring iconPath = argc > 1 ? std::filesystem::u8path(argv[1]).wstring() : L"assets/liukai.ico";
    const CLSID service_clsid = {0x01234567, 0x89ab, 0xcdef, {0x81,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}};
    auto *empty_action = new LanguageBarItem(service_clsid, {}, IconSource::from_path(iconPath));
    assert(empty_action->OnMenuSelect(static_cast<UINT>(MenuCommand::Traditional)) == E_UNEXPECTED);
    empty_action->Release();

    auto *pending_action = new LanguageBarItem(service_clsid, [](MenuCommand) { return E_PENDING; }, IconSource::from_path(iconPath));
    assert(pending_action->OnMenuSelect(static_cast<UINT>(MenuCommand::Japanese)) == E_PENDING);
    pending_action->Release();

    std::vector<MenuCommand> selected;
    auto *item = new LanguageBarItem(service_clsid, [&](MenuCommand command) { selected.push_back(command); return S_OK; }, IconSource::from_path(iconPath));
    ITfLangBarItem *base = nullptr;
    assert(item->QueryInterface(IID_ITfLangBarItem, reinterpret_cast<void **>(&base)) == S_OK);
    assert(base != nullptr);
    ITfLangBarItemButton *button = nullptr;
    assert(base->QueryInterface(kIidLangBarItemButton, reinterpret_cast<void **>(&button)) == S_OK);
    ITfSource *source = nullptr;
    assert(base->QueryInterface(IID_ITfSource, reinterpret_cast<void **>(&source)) == S_OK);

    TF_LANGBARITEMINFO info{};
    assert(base->GetInfo(&info) == S_OK);
    assert(IsEqualGUID(info.clsidService, service_clsid));
    TF_LANGBARITEMINFO button_info{};
    assert(button->GetInfo(&button_info) == S_OK);
    assert(IsEqualGUID(button_info.clsidService, service_clsid));
    assert(IsEqualGUID(info.guidItem, kGuidLbiInputMode));
    assert((info.dwStyle & TF_LBI_STYLE_BTN_MENU) != 0);
    assert(std::wstring(info.szDescription) == L"嘸．改");
    BSTR button_text = nullptr;
    assert(button->GetText(&button_text) == S_OK && button_text != nullptr);
    assert(std::wstring(button_text, SysStringLen(button_text)) == L"中");
    SysFreeString(button_text);
    BSTR tip = nullptr;
    assert(base->GetTooltipString(&tip) == S_OK && tip != nullptr);
    assert(SysStringLen(tip) > 0);
    SysFreeString(tip);

    FakeSink sink;
    DWORD cookie = 0;
    assert(source->AdviseSink(IID_ITfLangBarItemSink, &sink, &cookie) == S_OK);
    assert(cookie != 0 && sink.refs == 2);
    assert(item->update_display(LanguageBarMode::Japanese, true, false) == S_OK);
    assert(sink.updates.size() == 1);
    assert(button->GetText(&button_text) == S_OK && button_text != nullptr);
    assert(std::wstring(button_text, SysStringLen(button_text)) == L"英");
    SysFreeString(button_text);
    assert((sink.updates.front() & TF_LBI_BTNALL) == TF_LBI_BTNALL);
    assert(base->GetTooltipString(&tip) == S_OK);
    std::wstring tooltip(tip, SysStringLen(tip));
    SysFreeString(tip);
    assert(tooltip.find(L"日文") != std::wstring::npos);

    FakeMenu menu;
    assert(button->InitMenu(&menu) == S_OK);
    assert(menu.entries.size() == 11);
    assert(menu.entries[10].id == static_cast<UINT>(MenuCommand::Settings));
    assert(menu.entries[10].flags == 0);
    assert(menu.entries[9].id == static_cast<UINT>(MenuCommand::Lookup));
    assert(menu.entries[0].id == static_cast<UINT>(MenuCommand::Traditional));
    assert(menu.entries[1].id == static_cast<UINT>(MenuCommand::Simplified));
    assert(menu.entries[2].id == static_cast<UINT>(MenuCommand::TradToSimplified));
    assert(menu.entries[3].id == static_cast<UINT>(MenuCommand::Japanese));
    assert(menu.entries[3].flags == TF_LBMENUF_RADIOCHECKED);
    assert(menu.entries[4].id == static_cast<UINT>(MenuCommand::Fast));
    assert(menu.entries[5].id == static_cast<UINT>(MenuCommand::English));
    assert(menu.entries[6].id == static_cast<UINT>(MenuCommand::Chinese));
    assert(menu.entries[7].id == static_cast<UINT>(MenuCommand::Fullwidth));
    assert(menu.entries[8].id == static_cast<UINT>(MenuCommand::Halfwidth));
    assert(button->OnMenuSelect(static_cast<UINT>(MenuCommand::Fast)) == S_OK);
    assert(selected.size() == 1 && selected.front() == MenuCommand::Fast);
    assert(item->display_state().mode == LanguageBarMode::Japanese);

    assert(source->UnadviseSink(cookie) == S_OK && sink.refs == 1);
    assert(item->update_display(LanguageBarMode::Traditional, false, false) == S_OK);
    assert(sink.updates.size() == 1);

    HICON icon = nullptr;
    assert(button->GetIcon(&icon) == S_OK);
    assert(icon != nullptr);
    if (icon) assert(DestroyIcon(icon));
    button->Release();
    source->Release();
    base->Release();
    item->Release();
    assert(sink.refs == 1);
    std::cout << "language bar COM/menu/sink tests passed\n";
}
