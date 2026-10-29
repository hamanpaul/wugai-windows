#include <windows.h>
#include <objbase.h>
#include <iostream>
#include <functional>
#include <string>
#include <vector>
#include <filesystem>
#include "../installer/windows_backend.hpp"
#include "../installer/durable_transaction_observer.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return 1; } } while (0)
using namespace boshiamy::installer;

struct FakeSharedStore final : SharedResourceStore {
    std::array<ResourceState, 4> states{};
    Resource fail_apply = Resource::count;
    std::function<void()> on_tray_read;
    std::function<void()> on_prepare;
    std::function<void()> before_apply_failure;
    static size_t slot(Resource resource) {
        return resource == Resource::profile ? 0 : resource == Resource::keyboard_category ? 1 :
            resource == Resource::system_tray_category ? 2 : 3;
    }
    HRESULT read(Resource resource, ResourceState* state) override {
        if (!state) return E_POINTER;
        if (resource == Resource::system_tray_category && on_tray_read) { auto callback = std::move(on_tray_read); callback(); }
        *state = states[slot(resource)];
        return S_OK;
    }
    HRESULT prepare_apply_targets(TransactionOperation,const InstallRequest&,const ResourceStates&,ResourceStates* desired) override {
        if(!desired)return E_POINTER;
        if(on_prepare){auto callback=std::move(on_prepare);callback();}
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        if (resource == fail_apply) { if (before_apply_failure) { auto callback = std::move(before_apply_failure); callback(); } return E_ACCESSDENIED; }
        states[slot(resource)] = desired;
        return S_OK;
    }
    HRESULT restore(Resource resource, const ResourceState& prior) override {
        states[slot(resource)] = prior;
        return S_OK;
    }
};

static HRESULT create_view_root(const std::wstring& path, Resource view) {
    HKEY key = nullptr;
    LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
        KEY_READ | KEY_WRITE | WindowsInstallerBackend::view_flag(view), nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    RegCloseKey(key);
    return S_OK;
}

static HRESULT mark_view(const std::wstring& path, Resource view, const wchar_t* value) {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
        KEY_READ | KEY_WRITE | WindowsInstallerBackend::view_flag(view), &key);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key, L"InstallerTestViewMarker", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

static bool view_marker_is(const std::wstring& path, Resource view, const wchar_t* expected) {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
        KEY_READ | WindowsInstallerBackend::view_flag(view), &key);
    if (status != ERROR_SUCCESS) return false;
    wchar_t data[32]{}; DWORD bytes = sizeof(data), type = 0;
    status = RegQueryValueExW(key, L"InstallerTestViewMarker", nullptr, &type,
        reinterpret_cast<BYTE*>(data), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && std::wstring(data) == expected;
}

static void clean_view(const std::wstring& path, Resource view) {
    HKEY classes = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0,
        KEY_READ | KEY_WRITE | WindowsInstallerBackend::view_flag(view), &classes) == ERROR_SUCCESS) {
        const std::wstring relative = path.substr(std::wstring(L"Software\\Classes\\").size());
        RegDeleteTreeW(classes, relative.c_str());
        RegCloseKey(classes);
    }
}

static HRESULT read_server_path(const std::wstring& root_path, Resource view, const wchar_t* clsid, std::wstring* out) {
    HKEY root = nullptr, cls = nullptr, server = nullptr;
    HRESULT hr = WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER, root_path, view, KEY_READ, &root);
    if (FAILED(hr)) return hr;
    const std::wstring path = std::wstring(L"CLSID\\") + clsid;
    LONG status = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &cls);
    if (status == ERROR_SUCCESS) status = RegOpenKeyExW(cls, L"InprocServer32", 0, KEY_READ, &server);
    if (status == ERROR_SUCCESS) {
        wchar_t value[512]{}; DWORD bytes = sizeof(value), type = 0;
        status = RegQueryValueExW(server, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value), &bytes);
        if (status == ERROR_SUCCESS && type == REG_SZ) *out = value;
        else if (status == ERROR_SUCCESS) status = ERROR_INVALID_DATA;
    }
    if (server) RegCloseKey(server);
    if (cls) RegCloseKey(cls);
    RegCloseKey(root);
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

static HRESULT write_server_path(const std::wstring& root_path, Resource view, const wchar_t* clsid, const std::wstring& value) {
    HKEY root = nullptr, cls = nullptr, server = nullptr;
    HRESULT hr = WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER, root_path, view, KEY_READ | KEY_WRITE, &root);
    if (FAILED(hr)) return hr;
    const std::wstring path = std::wstring(L"CLSID\\") + clsid;
    LONG status = RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WRITE, &cls);
    if (status == ERROR_SUCCESS) status = RegOpenKeyExW(cls, L"InprocServer32", 0, KEY_READ | KEY_WRITE, &server);
    if (status == ERROR_SUCCESS) status = RegSetValueExW(server, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (server) RegCloseKey(server);
    if (cls) RegCloseKey(cls);
    RegCloseKey(root);
    return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

static HRESULT write_unknown_com_marker(const std::wstring& root_path,Resource view,const wchar_t* clsid) {
    HKEY root=nullptr,key=nullptr;
    HRESULT hr=WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,root_path,view,KEY_READ|KEY_WRITE,&root);
    if(FAILED(hr))return hr;
    const std::wstring path=std::wstring(L"CLSID\\")+clsid;
    LONG status=RegOpenKeyExW(root,path.c_str(),0,KEY_READ|KEY_WRITE,&key);
    const BYTE marker[]={0x4c,0x4b,0x7a,0x01};
    if(status==ERROR_SUCCESS)status=RegSetValueExW(key,L"InstallerUnknownMarker",0,REG_BINARY,marker,sizeof(marker));
    if(key) RegCloseKey(key);
    RegCloseKey(root);
    return status==ERROR_SUCCESS?S_OK:HRESULT_FROM_WIN32(status);
}
static bool unknown_com_marker_is(const std::wstring& root_path,Resource view,const wchar_t* clsid) {
    HKEY root=nullptr,key=nullptr;
    if(FAILED(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,root_path,view,KEY_READ,&root)))return false;
    const std::wstring path=std::wstring(L"CLSID\\")+clsid;
    LONG status=RegOpenKeyExW(root,path.c_str(),0,KEY_READ,&key);
    BYTE marker[4]{};DWORD size=sizeof(marker),type=0;
    if(status==ERROR_SUCCESS)status=RegQueryValueExW(key,L"InstallerUnknownMarker",nullptr,&type,marker,&size);
    if(key) RegCloseKey(key);
    RegCloseKey(root);
    const BYTE expected[]={0x4c,0x4b,0x7a,0x01};
    return status==ERROR_SUCCESS&&type==REG_BINARY&&size==sizeof(expected)&&std::memcmp(marker,expected,sizeof(expected))==0;
}

static bool write_pe_fixture(const std::wstring& path, WORD machine, WORD optional_magic) {
    const DWORD nt_offset = 0x80;
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = nt_offset;
    IMAGE_FILE_HEADER file{}; file.Machine = machine; file.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL;
    file.SizeOfOptionalHeader = machine == IMAGE_FILE_MACHINE_AMD64 ? sizeof(IMAGE_OPTIONAL_HEADER64) : sizeof(IMAGE_OPTIONAL_HEADER32);
    std::vector<BYTE> bytes(nt_offset + sizeof(DWORD) + sizeof(file) + file.SizeOfOptionalHeader, 0);
    std::memcpy(bytes.data(), &dos, sizeof(dos));
    const DWORD signature = IMAGE_NT_SIGNATURE;
    size_t offset = nt_offset; std::memcpy(bytes.data() + offset, &signature, sizeof(signature)); offset += sizeof(signature);
    std::memcpy(bytes.data() + offset, &file, sizeof(file)); offset += sizeof(file);
    std::memcpy(bytes.data() + offset, &optional_magic, sizeof(optional_magic));
    HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0; bool ok = WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(output); return ok;
}

static std::wstring temp_file(const wchar_t* suffix, const wchar_t* extension) {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t guid[40]{}; if (StringFromGUID2(id, guid, 40) <= 0) return {};
    wchar_t directory[MAX_PATH]{}; DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(directory); path += L"liukai-"; path += suffix; path += guid; path += extension; return path;
}

static bool real_com_install_with_durable_observer(WindowsInstallerBackend& backend,
    const InstallRequest& request,const InstallRequest& upgrade,const std::wstring& service_name,
    const std::wstring& classes_path) {
    GUID unique{}; if (FAILED(CoCreateGuid(&unique))) return false;
    wchar_t text[40]{}; if (StringFromGUID2(unique, text, 40) <= 0) return false;
    wchar_t temp[MAX_PATH]{}; const DWORD length=GetTempPathW(MAX_PATH,temp);
    if(length==0||length>=MAX_PATH)return false;
    std::wstring root(temp);root+=L"liukai-observer-";root+=text;
    if(!CreateDirectoryW(root.c_str(),nullptr))return false;
    struct Cleanup { std::wstring root; ~Cleanup(){DeleteFileW((root+L"\\.installer-journal").c_str());DeleteFileW((root+L"\\.installer-prestate").c_str());DeleteFileW((root+L"\\.installer-journal-upgrade").c_str());DeleteFileW((root+L"\\.installer-prestate-upgrade").c_str());RemoveDirectoryW(root.c_str());} } cleanup{root};
    namespace dj=boshiamy::installer::durable_journal;
    namespace snap=boshiamy::installer::durable_snapshot;
    namespace obs=boshiamy::installer::durable_observer;
    dj::Identity identity;identity.owner="LiuKai-TSF-v1";identity.token="real-com-observer";
    identity.install_root=root;identity.package_version="fixture-1";identity.package_manifest_sha256=std::string(64,'a');
    dj::Store journal(std::filesystem::path(root)/L".installer-journal",identity);
    snap::Store snapshot(std::filesystem::path(root)/L".installer-prestate",identity);
    obs::Observer observer(journal,snapshot,identity);
    const auto result=install_or_repair(backend,request,observer);
    std::cout<<"real COM observer operation="<<std::hex<<static_cast<unsigned long>(result.operation)
             <<" cleanup="<<static_cast<unsigned long>(result.cleanup)<<"\n";
    if(result.operation!=S_OK||result.cleanup!=S_OK)return false;
    auto all_events_match=[](dj::Store& store,const dj::Identity& expected) {
        dj::Document document;if(FAILED(store.read(expected,&document))||!document.all_operations_succeeded)return false;
        std::string desired;
        for(const auto& event:document.events) {
            if(event.kind==dj::Event::Kind::intent)desired=event.intent.desired_fingerprint;
            else if(event.kind==dj::Event::Kind::observation&&
                    (event.observation.operation_status!=S_OK||event.observation.observed_fingerprint!=desired))return false;
        }
        return true;
    };
    if(!all_events_match(journal,identity)) { std::cout<<"fresh COM journal observation mismatch\n";return false; }
    if(FAILED(write_unknown_com_marker(classes_path,Resource::com_x64,service_name.c_str())))return false;
    identity.token="real-com-upgrade";
    dj::Store upgrade_journal(std::filesystem::path(root)/L".installer-journal-upgrade",identity);
    snap::Store upgrade_snapshot(std::filesystem::path(root)/L".installer-prestate-upgrade",identity);
    obs::Observer upgrade_observer(upgrade_journal,upgrade_snapshot,identity);
    const auto upgraded=install_or_repair(backend,upgrade,upgrade_observer);
    if(upgraded.operation!=S_OK||upgraded.cleanup!=S_OK)return false;
    if(!all_events_match(upgrade_journal,identity)) { std::cout<<"upgrade COM journal observation mismatch\n";return false; }
    std::wstring current_path;
    return unknown_com_marker_is(classes_path,Resource::com_x64,service_name.c_str())&&
        SUCCEEDED(read_server_path(classes_path,Resource::com_x64,service_name.c_str(),&current_path))&&
        current_path==upgrade.x64_binary_path;
}

static HRESULT seed_fixture_com(const std::wstring& root_path,Resource resource,const wchar_t* clsid,
                                const std::wstring& owner,const std::wstring& dll) {
    HKEY root=nullptr,key=nullptr,server=nullptr;
    HRESULT hr=WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,root_path,resource,KEY_READ|KEY_WRITE,&root);
    if(FAILED(hr))return hr;
    LONG status=RegCreateKeyExW(root,(std::wstring(L"CLSID\\")+clsid).c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&key,nullptr);
    auto text=[](HKEY handle,const wchar_t* name,const std::wstring& value){return RegSetValueExW(handle,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));};
    if(status==ERROR_SUCCESS)status=text(key,L"LiuKaiOwner",owner);
    if(status==ERROR_SUCCESS)status=RegCreateKeyExW(key,L"InprocServer32",0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,&server,nullptr);
    if(status==ERROR_SUCCESS)status=text(server,nullptr,dll);
    if(status==ERROR_SUCCESS)status=text(server,L"ThreadingModel",L"Apartment");
    if(server)RegCloseKey(server);
    if(key)RegCloseKey(key);
    RegCloseKey(root);
    return status==ERROR_SUCCESS?S_OK:HRESULT_FROM_WIN32(status);
}

// Every handle here belongs to a uniquely named HKCU fixture. The machine
// resource is injected; these tests never open HKLM with write access.
static int machine_com_fixture_tests(const std::wstring& v1,const std::wstring& v2,
                                     const std::wstring& x86,const std::wstring& icon) {
    GUID fixture{},service{};CHECK(CoCreateGuid(&fixture)==S_OK&&CoCreateGuid(&service)==S_OK);
    wchar_t id[40]{},clsid[40]{};CHECK(StringFromGUID2(fixture,id,40)>0&&StringFromGUID2(service,clsid,40)>0);
    const std::wstring base=std::wstring(L"Software\\Classes\\CLSID\\")+id;
    const std::wstring user64=base+L"\\User64",user32=base+L"\\User32",machine=base+L"\\Machine64";
    struct Cleanup { std::wstring base;~Cleanup(){clean_view(base,Resource::com_x64);clean_view(base,Resource::com_x86);} } cleanup{base};
    CHECK(create_view_root(user64,Resource::com_x64)==S_OK);
    CHECK(create_view_root(user32,Resource::com_x86)==S_OK);
    CHECK(create_view_root(machine,Resource::machine_com_x64)==S_OK);
    CHECK(mark_view(machine,Resource::machine_com_x64,L"machine")==S_OK);
    CHECK(WindowsInstallerBackend::view_flag(Resource::machine_com_x64)==KEY_WOW64_64KEY);
    const std::wstring sid=L"S-1-5-21-1-2-3-1001";
    const std::wstring root=std::filesystem::path(v1).parent_path().wstring();
    FakeSharedStore shared;
    bool fail_delete=false;
    auto deletion=[&](HKEY handle,const std::wstring& path) {
        wchar_t marker[32]{};DWORD size=sizeof(marker),type=0;
        const bool is_machine=RegQueryValueExW(handle,L"InstallerTestViewMarker",nullptr,&type,reinterpret_cast<BYTE*>(marker),&size)==ERROR_SUCCESS&&type==REG_SZ&&std::wstring(marker)==L"machine";
        if(fail_delete&&is_machine) { fail_delete=false;RegDeleteTreeW(handle,(path+L"\\InprocServer32").c_str());return LONG(ERROR_ACCESS_DENIED); }
        return RegDeleteTreeW(handle,path.c_str());
    };
    auto open=[&](const std::wstring& bound_sid,const std::wstring& bound_root,bool writable=true) {
        HKEY a=nullptr,b=nullptr,c=nullptr;
        if(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,user64,Resource::com_x64,KEY_READ|KEY_WRITE,&a)!=S_OK)return std::unique_ptr<WindowsInstallerBackend>{};
        if(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,user32,Resource::com_x86,KEY_READ|KEY_WRITE,&b)!=S_OK){RegCloseKey(a);return std::unique_ptr<WindowsInstallerBackend>{};}
        if(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,machine,Resource::machine_com_x64,KEY_READ|KEY_WRITE,&c)!=S_OK){RegCloseKey(a);RegCloseKey(b);return std::unique_ptr<WindowsInstallerBackend>{};}
        return std::unique_ptr<WindowsInstallerBackend>(new WindowsInstallerBackend(a,b,service,L"LiuKai-TSF-v1",shared,deletion,c,bound_sid,bound_root,writable));
    };
    auto backend=open(sid,root);CHECK(backend&&backend->ready()==S_OK);
    CHECK(seed_fixture_com(user32,Resource::com_x86,clsid,L"Foreign-x86-owner",x86)==S_OK);
    const wchar_t* official=L"{11111111-1111-1111-1111-111111111111}";
    CHECK(seed_fixture_com(machine,Resource::machine_com_x64,official,L"Official-vendor",v1)==S_OK);
    InstallRequest legacy;legacy.owner=L"LiuKai-TSF-v1";legacy.x64_binary_path=v1;
    legacy.x86_binary_path=x86;legacy.icon_path=icon;legacy.include_x86=false;
    CHECK(install_or_repair(*backend,legacy).operation==S_OK);
    const auto shared_before=shared.states;
    ResourceState user_before,absent;CHECK(backend->read(Resource::com_x64,&user_before)==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&absent)==S_OK&&!absent.present);
    auto paired=legacy;paired.include_machine_com_x64=true;paired.expected_x64_path=v1;
    // Product owner alone cannot authorize an unbound machine key.
    CHECK(seed_fixture_com(machine,Resource::machine_com_x64,clsid,L"LiuKai-TSF-v1",v1)==S_OK);
    CHECK(install_or_repair(*backend,paired).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(backend->read(Resource::com_x64,&absent)==S_OK&&absent==user_before);
    HKEY fixture_root=nullptr;
    CHECK(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,machine,Resource::machine_com_x64,KEY_READ|KEY_WRITE,&fixture_root)==S_OK);
    CHECK(RegDeleteTreeW(fixture_root,(std::wstring(L"CLSID\\")+clsid).c_str())==ERROR_SUCCESS);RegCloseKey(fixture_root);
    // Force a later shared-resource failure after creating machine COM; rollback
    // must restore its original absence and exact user registration.
    shared.states[0]={};shared.fail_apply=Resource::profile;
    const auto failed_migration=install_or_repair(*backend,paired);
    CHECK(failed_migration.operation==E_ACCESSDENIED&&failed_migration.cleanup==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&absent)==S_OK&&!absent.present);
    CHECK(backend->read(Resource::com_x64,&absent)==S_OK&&absent==user_before);
    shared.states=shared_before;shared.fail_apply=Resource::count;
    CHECK(install_or_repair(*backend,paired).operation==S_OK);
    ResourceState machine_before,user_after,x86_before;
    CHECK(backend->read(Resource::machine_com_x64,&machine_before)==S_OK&&machine_before.created_by_lifecycle&&machine_before.path==v1);
    CHECK(backend->read(Resource::com_x64,&user_after)==S_OK&&user_after==user_before);
    CHECK(backend->read(Resource::com_x86,&x86_before)==S_OK&&x86_before.present&&x86_before.owner==L"Foreign-x86-owner");
    CHECK(shared.states==shared_before);
    CHECK(install_or_repair(*backend,paired).operation==S_OK);
    CHECK(install_or_repair(*backend,legacy).operation==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    auto wrong_sid=open(L"S-1-5-21-1-2-3-1002",root);
    CHECK(wrong_sid&&wrong_sid->read(Resource::machine_com_x64,&absent)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(!absent.created_by_lifecycle);
    CHECK(install_or_repair(*wrong_sid,paired).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    auto wrong_root=open(sid,root+L"\\different-root");
    CHECK(wrong_root&&wrong_root->read(Resource::machine_com_x64,&absent)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(write_unknown_com_marker(machine,Resource::machine_com_x64,clsid)==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&machine_before)==S_OK);
    ResourceState projected;CHECK(backend->project_apply_state(Resource::machine_com_x64,machine_before,&projected)==S_OK);
    CHECK(!projected.private_data.empty()&&projected.private_data!=machine_before.private_data);
    auto upgrade=paired;upgrade.x64_binary_path=v2;
    CHECK(install_or_repair(*backend,upgrade).operation==S_OK);
    ResourceState machine_upgraded,user_upgraded;
    CHECK(backend->read(Resource::machine_com_x64,&machine_upgraded)==S_OK&&machine_upgraded.path==v2);
    CHECK(backend->read(Resource::com_x64,&user_upgraded)==S_OK&&user_upgraded.path==v2);
    CHECK(unknown_com_marker_is(machine,Resource::machine_com_x64,clsid));
    auto read_only=open(sid,root,false);
    CHECK(read_only&&read_only->apply(Resource::machine_com_x64,machine_upgraded)==E_ACCESSDENIED);
    CHECK(read_only->restore(Resource::machine_com_x64,machine_before)==E_ACCESSDENIED);
    // A stale uninstall cannot remove either pointer.
    CHECK(FAILED(uninstall(*backend,paired).operation));
    ResourceState unchanged;CHECK(backend->read(Resource::machine_com_x64,&unchanged)==S_OK&&unchanged==machine_upgraded);
    // A machine-side race refuses its write and compensates the user pointer.
    shared.on_prepare=[&]{write_server_path(machine,Resource::machine_com_x64,clsid,v1);};
    auto repair=paired;repair.expected_x64_path=v2;
    const auto raced=install_or_repair(*backend,repair);
    CHECK(raced.operation==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&raced.cleanup==S_OK);
    CHECK(backend->read(Resource::com_x64,&unchanged)==S_OK&&unchanged==user_upgraded);
    CHECK(write_server_path(machine,Resource::machine_com_x64,clsid,v2)==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&machine_upgraded)==S_OK);
    // Partial deletion restores the exact machine raw tree and user pointer.
    auto remove=upgrade;remove.expected_x64_path=v2;
    fail_delete=true;
    const auto failed=uninstall(*backend,remove);
    CHECK(failed.operation==HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)&&failed.cleanup==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&unchanged)==S_OK&&unchanged==machine_upgraded);
    CHECK(backend->read(Resource::com_x64,&unchanged)==S_OK&&unchanged==user_upgraded);
    CHECK(uninstall(*backend,remove).operation==S_OK);
    CHECK(backend->read(Resource::machine_com_x64,&unchanged)==S_OK&&!unchanged.present);
    CHECK(backend->read(Resource::com_x64,&unchanged)==S_OK&&!unchanged.present);
    CHECK(backend->read(Resource::com_x86,&unchanged)==S_OK&&unchanged==x86_before);
    CHECK(shared.states==shared_before);
    std::wstring official_path;CHECK(read_server_path(machine,Resource::machine_com_x64,official,&official_path)==S_OK&&official_path==v1);
    CHECK(view_marker_is(machine,Resource::machine_com_x64,L"machine"));
    // Production opens the machine CLSID parent directly, so it does not need
    // write access to the broader Classes parent. Exercise creation/read/delete
    // and ensure a neighboring CLSID survives in an isolated HKCU fixture.
    HKEY scoped_user64=nullptr,scoped_user32=nullptr,scoped_machine=nullptr;
    CHECK(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,user64,Resource::com_x64,KEY_READ|KEY_WRITE,&scoped_user64)==S_OK);
    CHECK(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,user32,Resource::com_x86,KEY_READ|KEY_WRITE,&scoped_user32)==S_OK);
    CHECK(WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER,machine+L"\\CLSID",Resource::machine_com_x64,KEY_READ|KEY_WRITE,&scoped_machine)==S_OK);
    WindowsInstallerBackend scoped(scoped_user64,scoped_user32,service,L"LiuKai-TSF-v1",shared,{},scoped_machine,sid,root,true,true);
    auto scoped_request=legacy;scoped_request.include_machine_com_x64=true;
    CHECK(install_or_repair(scoped,scoped_request).operation==S_OK);
    CHECK(scoped.read(Resource::machine_com_x64,&unchanged)==S_OK&&unchanged.present&&unchanged.path==v1);
    CHECK(read_server_path(machine,Resource::machine_com_x64,official,&official_path)==S_OK&&official_path==v1);
    scoped_request.expected_x64_path=v1;
    const auto scoped_removed=uninstall(scoped,scoped_request);
    CHECK(scoped_removed.operation==S_OK&&scoped_removed.cleanup==S_OK);
    CHECK(scoped.read(Resource::machine_com_x64,&unchanged)==S_OK&&!unchanged.present);
    CHECK(read_server_path(machine,Resource::machine_com_x64,official,&official_path)==S_OK&&official_path==v1);
    return 0;
}

int main() {
    GUID test_id{}; CHECK(SUCCEEDED(CoCreateGuid(&test_id)));
    wchar_t id[40]{}; CHECK(StringFromGUID2(test_id, id, 40) > 0);
    const std::wstring classes_path = L"Software\\Classes";
    const std::wstring isolated = classes_path + L"\\CLSID\\" + std::wstring(id);
    struct Cleanup { std::wstring first, second; ~Cleanup() { for (const auto& path : {first, second}) if (!path.empty()) { clean_view(path, Resource::com_x64); clean_view(path, Resource::com_x86); } } } cleanup{isolated, L""};
    CHECK(SUCCEEDED(create_view_root(isolated, Resource::com_x64)));
    CHECK(SUCCEEDED(create_view_root(isolated, Resource::com_x86)));
    CHECK(SUCCEEDED(mark_view(isolated, Resource::com_x64, L"64")));
    CHECK(SUCCEEDED(mark_view(isolated, Resource::com_x86, L"32")));
    CHECK(view_marker_is(isolated, Resource::com_x64, L"64"));
    CHECK(view_marker_is(isolated, Resource::com_x86, L"32"));
    clean_view(isolated, Resource::com_x64); clean_view(isolated, Resource::com_x86);

    GUID service{}; CHECK(SUCCEEDED(CoCreateGuid(&service)));
    wchar_t service_name[40]{}; CHECK(StringFromGUID2(service, service_name, 40) > 0);
    cleanup.second = classes_path + L"\\CLSID\\" + std::wstring(service_name);
    FakeSharedStore shared;
    std::unique_ptr<WindowsInstallerBackend> backend;
    bool fail_delete_once = false;
    auto delete_hook = [&](HKEY classes, const std::wstring& clsid_path) -> LONG {
        if (fail_delete_once) {
            fail_delete_once = false;
            const std::wstring server_path = clsid_path + L"\\InprocServer32";
            RegDeleteTreeW(classes, server_path.c_str());
            return ERROR_ACCESS_DENIED;
        }
        return RegDeleteTreeW(classes, clsid_path.c_str());
    };
    CHECK(WindowsInstallerBackend::open_current_user(service, L"LiuKai-TSF-v1", shared, &backend, classes_path, delete_hook) == S_OK);
    CHECK(backend && backend->ready() == S_OK);
    const std::wstring x64_v1 = temp_file(L"x64-v1-", L".dll");
    const std::wstring x86_v1 = temp_file(L"x86-v1-", L".dll");
    const std::wstring x64_v2 = temp_file(L"x64-v2-", L".dll");
    const std::wstring x86_v2 = temp_file(L"x86-v2-", L".dll");
    const std::wstring icon = temp_file(L"icon-", L".ico");
    const std::wstring icon_v2 = temp_file(L"icon-v2-", L".ico");
    struct FileCleanup { std::vector<std::wstring> paths; ~FileCleanup() { for (const auto& path : paths) if (!path.empty()) DeleteFileW(path.c_str()); } } file_cleanup{{x64_v1,x86_v1,x64_v2,x86_v2,icon,icon_v2}};
    CHECK(write_pe_fixture(x64_v1, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC));
    CHECK(write_pe_fixture(x86_v1, IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC));
    CHECK(write_pe_fixture(x64_v2, IMAGE_FILE_MACHINE_AMD64, IMAGE_NT_OPTIONAL_HDR64_MAGIC));
    CHECK(write_pe_fixture(x86_v2, IMAGE_FILE_MACHINE_I386, IMAGE_NT_OPTIONAL_HDR32_MAGIC));
    HANDLE icon_file = CreateFileW(icon.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(icon_file != INVALID_HANDLE_VALUE); const BYTE icon_stub = 0; DWORD icon_written = 0;
    CHECK(WriteFile(icon_file, &icon_stub, sizeof(icon_stub), &icon_written, nullptr) && icon_written == sizeof(icon_stub)); CloseHandle(icon_file);
    icon_file = CreateFileW(icon_v2.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(icon_file != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(icon_file, &icon_stub, sizeof(icon_stub), &icon_written, nullptr) && icon_written == sizeof(icon_stub)); CloseHandle(icon_file);
    InstallRequest request{L"LiuKai-TSF-v1", x64_v1, x86_v1, icon, L"", L""};

    shared.fail_apply = Resource::profile;
    auto failed = install_or_repair(*backend, request);
    CHECK(FAILED(failed.operation)); CHECK(failed.cleanup == S_OK);
    std::wstring path;
    CHECK(FAILED(read_server_path(classes_path, Resource::com_x64, service_name, &path)));
    CHECK(FAILED(read_server_path(classes_path, Resource::com_x86, service_name, &path)));
    shared.fail_apply = Resource::count;
    auto wrong_arch = request; wrong_arch.x86_binary_path = x64_v1;
    CHECK(FAILED(backend->validate_install(wrong_arch)));
    CHECK(SUCCEEDED(backend->validate_install(request)));
    auto install_result = install_or_repair(*backend, request);
    CHECK(install_result.operation == S_OK);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v1);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x86, service_name, &path)) && path == x86_v1);

    auto changed = request; changed.x64_binary_path = x64_v2; changed.x86_binary_path = x86_v2;
    changed.expected_x64_path = x64_v1; changed.expected_x86_path = x86_v1;
    CHECK(install_or_repair(*backend, changed).operation == S_OK);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v2);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x86, service_name, &path)) && path == x86_v2);

    auto remove = changed;
    remove.expected_x64_path = x64_v2; remove.expected_x86_path = x86_v2;
    auto stale_remove = remove; stale_remove.expected_x64_path = x64_v1;
    auto stale_result = uninstall(*backend, stale_remove);
    CHECK(stale_result.operation == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v2);
    auto removed = uninstall(*backend, remove);
    CHECK(removed.operation == S_OK && removed.cleanup == S_OK);
    CHECK(FAILED(read_server_path(classes_path, Resource::com_x64, service_name, &path)));
    CHECK(FAILED(read_server_path(classes_path, Resource::com_x86, service_name, &path)));

    CHECK(install_or_repair(*backend, request).operation == S_OK);
    bool callback_ok = true;
    shared.on_tray_read = [&] { callback_ok = SUCCEEDED(write_server_path(classes_path, Resource::com_x64, service_name, x64_v2)); };
    auto raced = install_or_repair(*backend, changed);
    CHECK(callback_ok);
    CHECK(raced.operation == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    CHECK(raced.cleanup == S_OK);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v2);

    backend.reset();
    CHECK(WindowsInstallerBackend::open_current_user(service, L"LiuKai-TSF-v1", shared, &backend, classes_path, delete_hook) == S_OK);
    auto restore_v1 = request; restore_v1.expected_x64_path = x64_v2; restore_v1.expected_x86_path = x86_v1;
    CHECK(install_or_repair(*backend, restore_v1).operation == S_OK);
    auto rollback_case = changed;
    rollback_case.expected_x64_path = x64_v1; rollback_case.expected_x86_path = x86_v1;
    rollback_case.icon_path = icon_v2;
    shared.fail_apply = Resource::profile;
    callback_ok = true;
    shared.before_apply_failure = [&] { callback_ok = SUCCEEDED(write_server_path(classes_path, Resource::com_x64, service_name, x64_v1)); };
    auto rollback_conflict = install_or_repair(*backend, rollback_case);
    CHECK(callback_ok);
    CHECK(rollback_conflict.operation == E_ACCESSDENIED);
    CHECK(rollback_conflict.cleanup == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v1);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x86, service_name, &path)) && path == x86_v1);
    CHECK(shared.states[0].path == icon);

    backend.reset(); shared.fail_apply = Resource::count;
    CHECK(WindowsInstallerBackend::open_current_user(service, L"LiuKai-TSF-v1", shared, &backend, classes_path, delete_hook) == S_OK);
    auto final_remove = request; final_remove.expected_x64_path = x64_v1; final_remove.expected_x86_path = x86_v1;
    fail_delete_once = true;
    auto partial_delete = uninstall(*backend, final_remove);
    CHECK(partial_delete.operation == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    CHECK(partial_delete.cleanup == S_OK);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x64, service_name, &path)) && path == x64_v1);
    CHECK(SUCCEEDED(read_server_path(classes_path, Resource::com_x86, service_name, &path)) && path == x86_v1);
    CHECK(uninstall(*backend, final_remove).operation == S_OK);
    backend.reset();
    CHECK(WindowsInstallerBackend::open_current_user(service, L"LiuKai-TSF-v1", shared, &backend, classes_path, delete_hook) == S_OK);
    auto observed_upgrade=changed;observed_upgrade.expected_x64_path=x64_v1;observed_upgrade.expected_x86_path=x86_v1;
    CHECK(real_com_install_with_durable_observer(*backend, request, observed_upgrade, service_name, classes_path));
    auto observed_remove=observed_upgrade;observed_remove.expected_x64_path=x64_v2;observed_remove.expected_x86_path=x86_v2;
    CHECK(uninstall(*backend,observed_remove).operation==S_OK);
    CHECK(machine_com_fixture_tests(x64_v1,x64_v2,x86_v1,icon)==0);
    std::cout << "Windows backend isolated WOW64 registry transaction passed\n";
}
