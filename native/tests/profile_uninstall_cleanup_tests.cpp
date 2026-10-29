#include "../installer/profile_uninstall_cleanup.hpp"
#include <objbase.h>
#include <iostream>

namespace cleanup = boshiamy::installer::profile_uninstall;
#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " << __LINE__ << ": " #value "\n"; return false; } } while (false)
struct FakeStore final : cleanup::Store {
    cleanup::UserState user{true,1};
    bool machine = false, reappear = false;
    HRESULT read_status = S_OK, remove_status = S_OK;
    unsigned removals = 0;
    HRESULT machine_present(bool* out) override { *out=machine; return S_OK; }
    HRESULT read_user(cleanup::UserState* out) override { *out=user; return read_status; }
    HRESULT remove_user(const cleanup::UserState&) override {
        ++removals;
        if (remove_status==S_OK && !reappear) user={};
        return remove_status;
    }
};
static bool test_guards() {
    const cleanup::UserState before{true,1};
    FakeStore store;
    CHECK(cleanup::finish(store,before)==S_OK && store.removals==1 && !store.user.present);
    store={}; store.machine=true;
    CHECK(cleanup::finish(store,before)!=S_OK && store.removals==0);
    store={}; store.user.enabled=0;
    CHECK(cleanup::finish(store,before)!=S_OK && store.removals==0);
    store={};
    CHECK(cleanup::finish(store,{})!=S_OK && store.removals==0);
    store={}; store.user={};
    CHECK(cleanup::finish(store,before)==S_OK && store.removals==0);
    store={}; store.read_status=E_ACCESSDENIED;
    CHECK(cleanup::finish(store,before)==E_ACCESSDENIED && store.removals==0);
    store={}; store.remove_status=E_ACCESSDENIED;
    CHECK(cleanup::finish(store,before)==E_ACCESSDENIED && store.user.present);
    store={}; store.reappear=true;
    CHECK(cleanup::finish(store,before)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    return true;
}
static bool test_registry_leaf() {
    GUID id{}; CHECK(CoCreateGuid(&id)==S_OK);
    wchar_t guid[40]{}; CHECK(StringFromGUID2(id,guid,40)>0);
    const std::wstring root=std::wstring(L"Software\\LiuKai\\UninstallTests\\")+guid;
    struct Cleanup {
        std::wstring path;
        ~Cleanup() { if(path.rfind(L"Software\\LiuKai\\UninstallTests\\{",0)==0) RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str()); }
    } remove_fixture{root};
    const auto path=root+L"\\Profile";
    HKEY key=nullptr;
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,
        nullptr,&key,nullptr)==ERROR_SUCCESS);
    DWORD enabled=1;
    CHECK(RegSetValueExW(key,L"Enable",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS);
    cleanup::WindowsStore store(path);
    cleanup::UserState captured;
    CHECK(store.read_user(&captured)==S_OK && captured.present && captured.enabled==1);
    CHECK(RegSetValueExW(key,L"ForeignValue",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS);
    cleanup::UserState ignored;
    CHECK(store.read_user(&ignored)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    CHECK(RegDeleteValueW(key,L"ForeignValue")==ERROR_SUCCESS);
    HKEY child=nullptr;
    CHECK(RegCreateKeyExW(key,L"OtherProfile",0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&child,nullptr)==ERROR_SUCCESS);
    RegCloseKey(child);
    CHECK(store.read_user(&ignored)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    CHECK(RegDeleteKeyW(key,L"OtherProfile")==ERROR_SUCCESS);
    const wchar_t text[]=L"1";
    CHECK(RegSetValueExW(key,L"Enable",0,REG_SZ,reinterpret_cast<const BYTE*>(text),sizeof(text))==ERROR_SUCCESS);
    CHECK(store.read_user(&ignored)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    enabled=2;
    CHECK(RegSetValueExW(key,L"Enable",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS);
    CHECK(store.read_user(&ignored)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    enabled=1;
    CHECK(RegSetValueExW(key,L"Enable",0,REG_DWORD,reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS);
    RegCloseKey(key);
    CHECK(cleanup::finish(store,captured)==S_OK);
    CHECK(store.read_user(&ignored)==S_OK && !ignored.present);
    // The fixture parent is retained: cleanup removes only the exact leaf.
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER,root.c_str(),0,KEY_READ|KEY_WOW64_64KEY,&key)==ERROR_SUCCESS);
    RegCloseKey(key);
    return true;
}
int main() {
    if(!test_guards() || !test_registry_leaf()) return 1;
    std::cout << "profile uninstall residue guards and isolated HKCU leaf assertions passed\n";
    return 0;
}
