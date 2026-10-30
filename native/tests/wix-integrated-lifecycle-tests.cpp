#include "../wix/fresh-registration.hpp"
#include "../wix/modern-categories.hpp"
#include "../installer/durable_cli_session.hpp"
#include <iostream>
namespace i=boshiamy::installer;namespace c=i::package_categories;namespace m=i::msi_modern_categories;namespace j=i::durable_journal;namespace fs=std::filesystem;
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<'\n';return 1;}}while(0)
struct Backend final:i::Backend {
    i::ResourceStates state{};
    HRESULT read(i::Resource r,i::ResourceState* out)override{*out=state[static_cast<size_t>(r)];return S_OK;}
    HRESULT apply(i::Resource r,const i::ResourceState& s)override{state[static_cast<size_t>(r)]=s;return S_OK;}
    HRESULT restore(i::Resource r,const i::ResourceState& s)override{state[static_cast<size_t>(r)]=s;return S_OK;}
};
struct Categories final:c::Store {
    Backend& backend;c::Categories extras;
    explicit Categories(Backend& value):backend(value){}
    HRESULT read(c::Categories* out)override{
        *out=extras;
        if(backend.state[static_cast<size_t>(i::Resource::keyboard_category)].present)out->push_back(GUID_TFCAT_TIP_KEYBOARD);
        if(backend.state[static_cast<size_t>(i::Resource::system_tray_category)].present)out->push_back(c::tray);
        if(backend.state[static_cast<size_t>(i::Resource::ui_element_category)].present)out->push_back(c::ui);
        return S_OK;
    }
    HRESULT add(const GUID& id)override{if(c::contains(extras,id))return E_FAIL;extras.push_back(id);return S_OK;}
    HRESULT remove(const GUID& id)override{if(!c::contains(extras,id))return E_FAIL;extras.erase(std::remove_if(extras.begin(),extras.end(),[&](const GUID& x){return IsEqualGUID(x,id);}),extras.end());return S_OK;}
};
int main(){
    auto sandbox=fs::absolute(fs::path(L"build")/L"tests"/(L"wix-integrated-"+std::to_wstring(GetCurrentProcessId())));auto root=sandbox/L"LiuKai";
    auto base=sandbox/L".liukai-installer-ledger-machine-com-x64"/L"LiuKai-TSF-v1";
    auto installDir=base/L"install",removeDir=base/L"remove",restoreDir=base/L"restore",modernDir=sandbox/L"modern";
    fs::create_directories(installDir);fs::create_directories(modernDir);
    j::Identity install;install.schema_version=3;install.owner="LiuKai-TSF-v1";install.token="install";install.install_root=root.wstring();install.storage_root=installDir.wstring();install.package_version="test";install.package_manifest_sha256=std::string(64,'a');
    auto removed=install;removed.token="remove";removed.storage_root=removeDir.wstring();auto restored=install;restored.token="restore";restored.storage_root=restoreDir.wstring();
    j::Identity modern;modern.owner=install.owner;modern.token="modern";modern.install_root=modernDir.wstring();modern.package_version=install.package_version;modern.package_manifest_sha256=install.package_manifest_sha256;
    i::InstallRequest req;req.owner=L"LiuKai-TSF-v1";req.include_x86=false;req.include_machine_com_x64=true;
    req.x64_binary_path=(root/L"versions"/L"test"/L"x64"/L"BoshiamyTsf.dll").wstring();req.icon_path=(root/L"shared"/L"branding.dll").wstring();
    Backend backend;Categories cats(backend);auto result=i::msi_registration::install_fresh_durable(backend,req,install);
    CHECK(result.operation==S_OK&&result.cleanup==S_OK);
    CHECK(i::durable_cli::detail::inspect_machine_history(root,"LiuKai-TSF-v1")==S_OK);
    c::Categories baseline;CHECK(cats.read(&baseline)==S_OK&&baseline.size()==3);
    j::Store added(modernDir/L"added.journal",modern);m::DurableObserver addLog(added,modern);
    CHECK(m::transition(cats,baseline,true,addLog)==S_OK&&m::inspect_committed(cats,added,modern,true)==S_OK);
    c::Categories full;CHECK(cats.read(&full)==S_OK&&full.size()==5);
    j::Store removedExtras(modernDir/L"removed.journal",modern);m::DurableObserver removeLog(removedExtras,modern);
    CHECK(m::transition(cats,full,false,removeLog)==S_OK);
    fs::create_directories(removeDir);
    req.expected_x64_path=req.x64_binary_path;
    result=i::msi_registration::rollback_committed_fresh(backend,req,install,removed);CHECK(result.operation==S_OK&&result.cleanup==S_OK);
    for(const auto& state:backend.state)CHECK(state==i::ResourceState{});
    fs::create_directories(restoreDir);
    result=i::msi_registration::restore_committed_removal(backend,install,removed,restored);CHECK(result.operation==S_OK&&result.cleanup==S_OK);
    CHECK(i::durable_cli::detail::inspect_machine_history(root,"LiuKai-TSF-v1")==S_OK);
    CHECK(m::inspect_committed(cats,removedExtras,modern,false)==S_OK);
    j::Store restoredExtras(modernDir/L"restored.journal",modern);m::DurableObserver restoreLog(restoredExtras,modern);
    CHECK(cats.read(&baseline)==S_OK&&m::transition(cats,baseline,true,restoreLog)==S_OK);
    CHECK(m::inspect_committed(cats,restoredExtras,modern,true)==S_OK);
    for(const auto& dir:{installDir,removeDir,restoreDir}){fs::remove(dir/L"transaction.journal");fs::remove(dir/L"transaction.snapshot");fs::remove(dir/L"transaction.final.snapshot");fs::remove(dir);}
    fs::remove(base);fs::remove(base.parent_path());
    for(const auto& name:{L"added.journal",L"removed.journal",L"restored.journal"})fs::remove(modernDir/name);
    fs::remove(modernDir);fs::remove(sandbox);std::cout<<"integrated_lifecycle_tests=passed\n";
}
