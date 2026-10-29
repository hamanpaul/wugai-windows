#include "../installer/package_uninstall.hpp"
#include "../installer/deployment_coordinator.hpp"
#include <fstream>
#include <iostream>
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<"\n";return false;}}while(0)
namespace fs=std::filesystem;
namespace i=boshiamy::installer;
namespace f=i::files;
namespace d=i::durable_cli;
namespace c=i::package_categories;
namespace p=i::package_uninstall;
struct Backend final:i::Backend {
    i::ResourceStates states{};size_t writes=0;
    HRESULT read_error=S_OK;
    bool fail_apply_once=false;
    HRESULT read(i::Resource resource,i::ResourceState* out)override {
        if(read_error!=S_OK)return read_error;
        *out=states[static_cast<size_t>(resource)];return S_OK;
    }
    HRESULT apply(i::Resource resource,const i::ResourceState& target)override {
        if(fail_apply_once){fail_apply_once=false;++writes;return E_ACCESSDENIED;}
        ++writes;states[static_cast<size_t>(resource)]=target;return S_OK;
    }
    HRESULT restore(i::Resource resource,const i::ResourceState& target)override{return apply(resource,target);}
    HRESULT verify_install_health()override{return S_OK;}
    HRESULT prepare_apply_targets(i::TransactionOperation,const i::InstallRequest& request,
            const i::ResourceStates&,i::ResourceStates* desired)override {
        auto& machine=(*desired)[static_cast<size_t>(i::Resource::machine_com_x64)];
        if(request.include_machine_com_x64&&machine.present&&machine.private_data.empty())
            machine.private_data={'S','I','D','r','o','o','t',0xff};
        return S_OK;
    }
    HRESULT project_apply_state(i::Resource resource,const i::ResourceState& state,i::ResourceState* out)const override {
        *out=state;
        if(resource==i::Resource::machine_com_x64&&state.present) {
            if(state.private_data.size()<7)return E_INVALIDARG;
            out->private_data.resize(7);
        }
        return S_OK;
    }
};
struct Categories final:c::Store {
    Backend* backend; c::Categories extra;
    HRESULT read_error=S_OK,remove_error=S_OK;
    size_t writes=0,fail_on_remove=0; bool pretend_remove=false;
    explicit Categories(Backend& b):backend(&b){}
    HRESULT read(c::Categories* out)override {
        if(read_error!=S_OK)return read_error;
        *out=extra;
        const std::pair<i::Resource,GUID> rows[]={{i::Resource::keyboard_category,GUID_TFCAT_TIP_KEYBOARD},
            {i::Resource::system_tray_category,c::tray},{i::Resource::ui_element_category,c::ui}};
        for(const auto& row:rows)if(backend->states[static_cast<size_t>(row.first)].present)out->push_back(row.second);
        return S_OK;
    }
    HRESULT remove(const GUID& id)override {
        ++writes;if(fail_on_remove==writes)return E_ACCESSDENIED;
        if(remove_error!=S_OK)return remove_error;
        if(!pretend_remove)extra.erase(std::remove_if(extra.begin(),extra.end(),[&](const GUID& x){return IsEqualGUID(x,id);}),extra.end());
        return S_OK;
    }
};
static bool deploy(const fs::path& package,const fs::path& root,Backend& backend,d::journal::Identity* identity) {
    f::DeploymentRequest request;request.package_root=package;request.install_root=root;
    request.owner="LiuKai-TSF-v1";request.include_machine_com_x64=true;
    const i::DeploymentObserverFactory factory=[&](const f::DeploymentRequest& input,const f::DeploymentPaths& paths,
            std::unique_ptr<i::TransactionObserver>* out) {
        const HRESULT result=d::Session::create(input,paths,out,[&](i::Resource resource,const i::ResourceState& state,i::ResourceState* target){
            return backend.project_apply_state(resource,state,target);});
        if(result==S_OK)*identity=dynamic_cast<d::Session*>(out->get())->identity();
        return result;
    };
    const auto result=i::deploy_and_register(backend,request,nullptr,factory);
    CHECK(result.operation==S_OK&&result.cleanup==S_OK);return true;
}
static size_t entries(const fs::path& root) {
    size_t count=0;for(const auto& row:fs::recursive_directory_iterator(root)){(void)row;++count;}return count;
}
static bool scenario(const fs::path& package,const fs::path& sandbox) {
    const auto root=sandbox/L"install";
    Backend backend;Categories categories(backend);d::journal::Identity identity;
    CHECK(deploy(package,root,backend,&identity));
    categories.extra={c::immersive,c::input_mode};
    const auto original=backend.states;const size_t initial_writes=backend.writes;
    p::Report plan;const auto ledger=fs::path(identity.storage_root).parent_path();
    const size_t history_entries=entries(ledger);
    auto result=p::run(backend,root,false,&plan,&categories);
    if(result.operation!=S_OK)std::cerr<<"plan hr="<<std::hex<<result.operation<<" stage="<<plan.failure_stage<<std::dec<<"\n";
    CHECK(result.operation==S_OK&&result.cleanup==S_OK&&!plan.executed&&!plan.registrations_absent);
    CHECK(backend.writes==initial_writes&&categories.writes==0&&entries(ledger)==history_entries);
    CHECK(plan.inventory.size()>=16&&plan.inventory_fingerprint.size()==64);
    CHECK(std::any_of(plan.inventory.begin(),plan.inventory.end(),[](const p::File& x){return x.relative_path==".liukai-owner";}));
    CHECK(std::any_of(plan.inventory.begin(),plan.inventory.end(),[](const p::File& x){return x.relative_path.find("/package-manifest.json")!=std::string::npos;}));

    // Ownership and read failures refuse both planning and mutation.
    for(const int variant:{0,1,2,3,4}) {
        backend.states=original;
        auto& com=backend.states[static_cast<size_t>(i::Resource::machine_com_x64)];
        if(variant==0)com.owner=L"foreign";
        if(variant==1)com.created_by_lifecycle=false;
        if(variant==2)com.path+=L".other-root";
        if(variant==3)backend.states[static_cast<size_t>(i::Resource::com_x86)]={true,true,L"LiuKai-TSF-v1",L"C:\\old.dll",L"Apartment",{}};
        if(variant==4)backend.read_error=HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS); // scoped backend wrong SID/root
        p::Report refused;result=p::run(backend,root,true,&refused,&categories);
        CHECK(result.operation!=S_OK&&backend.writes==initial_writes&&categories.writes==0);
        CHECK(entries(ledger)==history_entries);
        backend.read_error=S_OK;
    }
    backend.states=original;
    const GUID unknown={0x12345678,0x1234,0x1234,{1,2,3,4,5,6,7,8}};
    categories.extra.push_back(unknown);
    result=p::run(backend,root,true,&plan,&categories);
    CHECK(result.operation!=S_OK&&plan.failure_stage=="category-ownership"&&backend.writes==initial_writes);
    categories.extra.pop_back();

    // Unknown root and version files are never authorized by an inventory.
    for(const fs::path extra:{root/L"user-data.txt",root/L"versions"/f::detail::widen_ascii(plan.installed.version)/L"extra.txt"}) {
        {std::ofstream out(extra);out<<"keep";}
        CHECK(p::run(backend,root,true,&plan,&categories).operation!=S_OK);
        CHECK(backend.writes==initial_writes&&fs::is_regular_file(extra));CHECK(fs::remove(extra));
    }
    CHECK(fs::create_directory(root/L".txn-unknown"));
    CHECK(p::run(backend,root,true,&plan,&categories).operation!=S_OK);
    CHECK(fs::remove(root/L".txn-unknown"));

    // Older managed versions are included and independently validated.
    const auto current=root/L"versions"/f::detail::widen_ascii(plan.installed.version);
    const auto older=root/L"versions"/L"older-fixture";
    fs::copy(current,older,fs::copy_options::recursive);
    std::string manifest=f::detail::read_text(older/L"package-manifest.json");
    size_t at=0;
    while((at=manifest.find(plan.installed.version,at))!=std::string::npos){manifest.replace(at,plan.installed.version.size(),"older-fixture");at+=13;}
    {std::ofstream out(older/L"package-manifest.json",std::ios::binary|std::ios::trunc);out<<manifest;}
    {std::ofstream out(older/L".liukai-version-owner",std::ios::binary|std::ios::trunc);out<<f::detail::version_owner_contents("LiuKai-TSF-v1","older-fixture");}
    CHECK(p::run(backend,root,false,&plan,&categories).operation==S_OK);
    CHECK(std::any_of(plan.inventory.begin(),plan.inventory.end(),[](const p::File& x){return x.relative_path=="versions/older-fixture/package-manifest.json";}));
    {std::ofstream out(older/L"x64"/L"dictionary.tsv",std::ios::binary|std::ios::app);out<<"drift";}
    CHECK(p::run(backend,root,true,&plan,&categories).operation!=S_OK&&backend.writes==initial_writes);
    fs::copy_file(current/L"x64"/L"dictionary.tsv",older/L"x64"/L"dictionary.tsv",fs::copy_options::overwrite_existing);

    // A valid but unresolved transaction must be preserved and refuse removal.
    i::InstallRequest remove;remove.owner=L"LiuKai-TSF-v1";remove.include_x86=false;remove.include_machine_com_x64=true;
    remove.expected_x64_path=(current/L"x64"/L"BoshiamyTsf.dll").wstring();
    std::unique_ptr<i::TransactionObserver> pending;
    CHECK(d::Session::create_uninstall(remove,root,&pending,[&](i::Resource r,const i::ResourceState& s,i::ResourceState* t){return backend.project_apply_state(r,s,t);})==S_OK);
    auto* pending_session=dynamic_cast<d::Session*>(pending.get());CHECK(pending_session);
    const auto pending_path=fs::path(pending_session->identity().storage_root);
    CHECK(pending->begin(i::TransactionOperation::uninstall,backend.states)==S_OK);
    pending.reset();
    const auto pending_hash=f::detail::sha256_file(pending_path/L"transaction.journal");
    CHECK(p::run(backend,root,true,&plan,&categories).operation!=S_OK&&plan.failure_stage=="durable-history");
    CHECK(backend.writes==initial_writes&&f::detail::sha256_file(pending_path/L"transaction.journal")==pending_hash);
    // Test-owned sandbox only: retire the deliberately abandoned fixture so the
    // independent successful lifecycle below can run with its original history.
    CHECK(pending_path.parent_path()==ledger&&pending_path!=fs::path(identity.storage_root));
    CHECK(fs::remove_all(pending_path)>0);

    // Partial modern cleanup creates no pending ledger and retains owned
    // pointers/profile, so repeating the package operation is safe.
    categories.fail_on_remove=2;
    result=p::run(backend,root,true,&plan,&categories);
    CHECK(result.operation==E_ACCESSDENIED&&plan.modern_category_cleanup_started);
    CHECK(backend.states==original&&backend.writes==initial_writes&&categories.extra.size()==1);
    CHECK(entries(ledger)==history_entries);
    categories.fail_on_remove=0;
    // Failure in the durable seven-resource phase restores those resources,
    // retains a valid compensation receipt, and reports the category progress.
    backend.fail_apply_once=true;
    result=p::run(backend,root,true,&plan,&categories);
    CHECK(result.operation==E_ACCESSDENIED&&result.cleanup==S_OK&&plan.modern_category_cleanup_started);
    CHECK(backend.states==original&&categories.extra.empty()&&!plan.registrations_absent);
    CHECK(fs::is_regular_file(root/L"LiuKaiInstaller.exe"));
    CHECK(p::run(backend,root,false,&plan,&categories).operation==S_OK);
    const auto fingerprint=plan.inventory_fingerprint;
    p::Report removed;result=p::run(backend,root,true,&removed,&categories);
    if(result.operation!=S_OK||result.cleanup!=S_OK)std::cerr<<"remove hr="<<std::hex<<result.operation<<" cleanup="<<result.cleanup<<" stage="<<removed.failure_stage<<std::dec<<"\n";
    CHECK(result.operation==S_OK&&result.cleanup==S_OK&&removed.executed&&removed.registrations_absent);
    CHECK(backend.states==i::ResourceStates{}&&categories.extra.empty()&&categories.writes==3);
    CHECK(removed.inventory_fingerprint==fingerprint&&fs::is_regular_file(root/L"LiuKaiInstaller.exe"));
    CHECK(entries(ledger)>history_entries);
    // A file-phase failure leaves the package tree in place. Native removal
    // must be safely repeatable with all registration already absent.
    CHECK(p::run(backend,root,false,&plan,&categories).operation==S_OK);
    CHECK(p::run(backend,root,true,&removed,&categories).operation==S_OK&&removed.registrations_absent);
    CHECK(d::detail::inspect_machine_history(root,"LiuKai-TSF-v1",[&](i::Resource r,const i::ResourceState& s,i::ResourceState* t){return backend.project_apply_state(r,s,t);})==S_OK);
    // Simulate the package wrapper's file phase only inside this new sandbox.
    CHECK(root.parent_path()==sandbox&&root.filename()==L"install");
    CHECK(fs::remove_all(root)>0);
    CHECK(deploy(package,root,backend,&identity));
    return true;
}
static bool category_failures() {
    Backend backend;Categories store(backend);store.extra={c::immersive,c::input_mode};
    const auto before=store.extra;
    store.pretend_remove=true;
    CHECK(c::remove_extras(store,before)!=S_OK&&store.writes==1&&store.extra.size()==2);
    store.pretend_remove=false;store.writes=0;store.remove_error=E_ACCESSDENIED;
    CHECK(c::remove_extras(store,before)==E_ACCESSDENIED&&store.extra.size()==2);
    store.remove_error=S_OK;store.extra={c::ui};
    CHECK(c::remove_extras(store,before)!=S_OK);
    store.extra={c::input_mode};
    CHECK(c::remove_extras(store,{c::immersive})!=S_OK);
    store.extra={c::immersive};store.read_error=S_FALSE;
    CHECK(c::remove_extras(store,before)==S_FALSE);
    return true;
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    const fs::path package=argv[1],sandbox=argv[2];
    if(!package.is_absolute()||!sandbox.is_absolute()||fs::exists(sandbox))return 2;
    if(!fs::create_directories(sandbox))return 2;
    if(!category_failures()||!scenario(package,sandbox))return 1;
    std::cout<<"package uninstall inventory, ownership, history, modern categories, and reinstall assertions passed\n";
    return 0;
}
