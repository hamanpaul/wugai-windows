#include "../installer/tsf_shared_store.hpp"
#include "../installer/durable_cli_session.hpp"
#include "../installer/dev_host_migration_history.hpp"
#include <iostream>
#include <memory>
using namespace boshiamy::installer;
int main() {
    HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(init))return 2;
    wchar_t directory[32768]{};
    DWORD n=GetEnvironmentVariableW(L"ProgramFiles",directory,32768);
    if(!n||n>=32768){CoUninitialize();return 2;}
    const std::wstring root=(std::filesystem::path(directory)/L"LiuKai").wstring();
    HRESULT machine=E_FAIL,user=E_FAIL;
    {
        WindowsTsfSharedApi api;
        RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,L"LiuKai-TSF-v1");
        WindowsTsfSharedStore shared(api,markers,L"LiuKai-TSF-v1");
        std::unique_ptr<WindowsInstallerBackend> backend;
        machine=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,L"LiuKai-TSF-v1",shared,&backend,true,root,KEY_READ);
        if(machine==S_OK) {
            auto projection=[&backend](Resource r,const ResourceState& s,ResourceState* out){return backend->project_apply_state(r,s,out);};
            machine=durable_cli::detail::inspect_machine_history(root,"LiuKai-TSF-v1",projection);
            user=dev_host_migration_history::inspect(root);
        }
    }
    CoUninitialize();
    std::cout<<"{\"kind\":\"liukai-history-preflight-v1\",\"machine_history\":"<<static_cast<uint32_t>(machine)<<",\"legacy_history\":"<<static_cast<uint32_t>(user)<<"}\n";
    return machine==S_OK&&user==S_OK?0:1;
}
