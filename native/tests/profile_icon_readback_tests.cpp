#include "../installer/tsf_shared_store.hpp"
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return 1; } } while (0)

namespace detail = boshiamy::profile_display::windows_detail;

struct Fixture {
    std::wstring path = L"Software\\LiuKai\\Tests\\ProfileIconReadback-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY key = nullptr;
    DWORD disposition = 0;
    ~Fixture() {
        if (key) RegCloseKey(key);
        if (disposition == REG_CREATED_NEW_KEY)
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
    LONG put(const wchar_t* name, DWORD type, const void* value, DWORD bytes) {
        return RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(value), bytes);
    }
    HRESULT read(std::wstring* out) { return detail::read_profile_icon_path(HKEY_CURRENT_USER, path, out); }
};

int wmain(int argc, wchar_t** argv) {
    // This production-path check only reads the existing TSF profile. It is
    // useful on a machine affected by the R60 failure, without reinstallation.
    if (argc == 2 && std::wstring(argv[1]) == L"--live-readonly") {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 1;
        HRESULT result;
        {
            boshiamy::installer::WindowsTsfSharedApi api;
            std::wstring path;
            result = api.profile_icon_path(&path);
            std::wcout << L"icon_readback=" << static_cast<unsigned long>(result)
                       << L" path=" << path << L"\n";
            if (result == S_OK && path.empty()) result = E_UNEXPECTED;
        }
        CoUninitialize();
        return result == S_OK ? 0 : 1;
    }
    if (argc != 1) return 2;
    Fixture f;
    std::wstring observed = L"stale";
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    CHECK(observed.empty());
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, f.path.c_str(), 0, nullptr, 0,
        KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &f.key, &f.disposition) == ERROR_SUCCESS);
    CHECK(f.disposition == REG_CREATED_NEW_KEY);
    CHECK(f.read(nullptr) == E_POINTER);
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));

    const std::wstring expected = L"C:\\Program Files\\LiuKai\\shared\\branding.dll";
    CHECK(f.put(L"IconFile", REG_SZ, expected.c_str(),
        static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    // Neither settings DWORD exists, as on a fresh R60 installation. No other
    // metadata or COM registration is required to read this exact binding.
    CHECK(f.read(&observed) == S_OK && observed == expected);
    const wchar_t unrelated[] = L"not a DWORD";
    CHECK(f.put(L"HiddenInSettingUI", REG_SZ, unrelated, sizeof(unrelated)) == ERROR_SUCCESS);
    CHECK(f.read(&observed) == S_OK && observed == expected);

    const DWORD wrong_type = 1;
    CHECK(f.put(L"IconFile", REG_DWORD, &wrong_type, sizeof(wrong_type)) == ERROR_SUCCESS);
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && observed.empty());
    const wchar_t embedded_null[] = {L'a', L'\0', L'b', L'\0'};
    CHECK(f.put(L"IconFile", REG_SZ, embedded_null, sizeof(embedded_null)) == ERROR_SUCCESS);
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && observed.empty());
    const wchar_t empty[] = L"";
    CHECK(f.put(L"IconFile", REG_SZ, empty, sizeof(empty)) == ERROR_SUCCESS);
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && observed.empty());
    CHECK(RegDeleteValueW(f.key, L"IconFile") == ERROR_SUCCESS);
    CHECK(f.read(&observed) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && observed.empty());
    std::cout << "profile icon readback: sparse metadata and invalid bindings passed\n";
    return 0;
}
