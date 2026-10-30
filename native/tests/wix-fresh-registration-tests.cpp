#include "../wix/fresh-registration.hpp"
#include <iostream>
using namespace boshiamy::installer;
#define CHECK(v) do { if(!(v)) { std::cerr << #v << " at " << __LINE__ << '\n'; return 1; } } while(0)
struct MemoryBackend : Backend {
    ResourceStates states{}; int writes=0; Resource fail=Resource::count;
    HRESULT read(Resource r,ResourceState* out) override {*out=states[static_cast<size_t>(r)]; return S_OK;}
    HRESULT apply(Resource r,const ResourceState& s) override {
        ++writes; if(r==fail)return E_ACCESSDENIED; states[static_cast<size_t>(r)]=s;
        if(r==Resource::profile&&s.present)states[static_cast<size_t>(r)].private_data={0x42,0x4c,0x53,0x55,0x31,1,0};
        return S_OK;
    }
    HRESULT restore(Resource r,const ResourceState& s) override {if(r==fail)return E_ACCESSDENIED;states[static_cast<size_t>(r)]=s;return S_OK;}
    HRESULT prepare_apply_targets(TransactionOperation op,const InstallRequest&,const ResourceStates& before,ResourceStates* desired) override {
        if(op==TransactionOperation::uninstall)for(size_t i=0;i<desired->size();++i)
            if(before[i].present&&!(*desired)[i].present)(*desired)[i]=ResourceState{};
        return S_OK;
    }
    HRESULT project_apply_state(Resource r,const ResourceState& s,ResourceState* out)const override {*out=s;if(r==Resource::profile)out->private_data.clear();return S_OK;}
    HRESULT verify_applied(Resource r,const ResourceState& desired,const ResourceState& observed)const override {
        if(r!=Resource::profile)return Backend::verify_applied(r,desired,observed);
        auto adjusted=observed;adjusted.private_data=desired.private_data;return adjusted==desired?S_OK:E_FAIL;
    }
};
struct Recorder : TransactionObserver {
    int begins=0,commits=0;
    HRESULT begin(TransactionOperation,const ResourceStates&) override {++begins;return S_OK;}
    HRESULT before(Resource,TransactionMutation,const ResourceState&,const ResourceState&) override {return S_OK;}
    HRESULT after(Resource,TransactionMutation,HRESULT,HRESULT,const ResourceState*,bool) override {return S_OK;}
    HRESULT commit(const ResourceStates&) override {++commits;return S_OK;}
};
int main() {
    InstallRequest request; request.owner=L"LiuKai-TSF-v1";request.include_x86=false;request.include_machine_com_x64=true;
    request.x64_binary_path=L"C:\\Program Files\\LiuKai\\versions\\test\\x64\\BoshiamyTsf.dll";
    request.icon_path=L"C:\\Program Files\\LiuKai\\shared\\branding.dll";
    MemoryBackend clean;Recorder log;
    auto result=msi_registration::install_fresh(clean,request,log);
    CHECK(result.operation==S_OK && result.cleanup==S_OK && log.begins==1 && log.commits==1);
    CHECK(clean.states[static_cast<size_t>(Resource::machine_com_x64)].present);
    MemoryBackend conflict;Recorder conflictLog;
    conflict.states[static_cast<size_t>(Resource::profile)].present=true;
    result=msi_registration::install_fresh(conflict,request,conflictLog);
    CHECK(result.operation!=S_OK && conflict.writes==0 && conflictLog.begins==0);
    MemoryBackend failing;Recorder failedLog;failing.fail=Resource::profile;
    result=msi_registration::install_fresh(failing,request,failedLog);
    CHECK(result.operation==E_ACCESSDENIED && result.cleanup==S_OK && failedLog.commits==0);
    for(const auto& state:failing.states)CHECK(state==ResourceState{});
    namespace fs=std::filesystem;
    auto sandbox=fs::absolute(fs::path(L"build")/L"tests"/(L"wix-durable-"+std::to_wstring(GetCurrentProcessId())));
    auto root=sandbox/L"LiuKai";
    durable_journal::Identity id;id.schema_version=3;id.owner="LiuKai-TSF-v1";id.token="test-01";
    id.package_version="test";id.package_manifest_sha256=std::string(64,'a');id.install_root=root.wstring();
    auto ledger=sandbox/durable_journal::detail::ledger_namespace(id)/L"LiuKai-TSF-v1"/L"test-01";
    id.storage_root=ledger.wstring();fs::create_directories(ledger);
    request.x64_binary_path=(root/L"versions"/L"test"/L"x64"/L"BoshiamyTsf.dll").wstring();
    request.icon_path=(root/L"shared"/L"branding.dll").wstring();
    MemoryBackend durableBackend;
    result=msi_registration::install_fresh_durable(durableBackend,request,id);
    CHECK(result.operation==S_OK && result.cleanup==S_OK);
    durable_journal::Store journal(ledger/L"transaction.journal",id,ledger);
    durable_journal::Document document;
    CHECK(journal.read(id,&document)==S_OK && document.committed && document.all_operations_succeeded);
    CHECK(fs::is_regular_file(ledger/L"transaction.snapshot"));
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)==S_OK);
    auto original=durableBackend.states[static_cast<size_t>(Resource::profile)];
    durableBackend.states[static_cast<size_t>(Resource::profile)].private_data.back()=1;
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)==S_OK);
    durableBackend.states[static_cast<size_t>(Resource::profile)]=original;
    durableBackend.states[static_cast<size_t>(Resource::profile)].private_data[0]^=1;
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)!=S_OK);
    durableBackend.states[static_cast<size_t>(Resource::profile)]=original;
    durableBackend.states[static_cast<size_t>(Resource::profile)].path=L"C:\\foreign\\other.dll";
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)!=S_OK);
    durableBackend.states[static_cast<size_t>(Resource::profile)]=original;
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)==S_OK);
    MemoryBackend retry;
    result=msi_registration::install_fresh_durable(retry,request,id);
    CHECK(result.operation!=S_OK && retry.writes==0);
    auto undo=id;undo.token="test-02";auto undoLedger=ledger.parent_path()/L"test-02";undo.storage_root=undoLedger.wstring();fs::create_directories(undoLedger);
    request.expected_x64_path=request.x64_binary_path;
    durableBackend.states[static_cast<size_t>(Resource::profile)].path=L"C:\\foreign\\other.dll";
    int writes=durableBackend.writes;
    result=msi_registration::rollback_committed_fresh(durableBackend,request,id,undo);
    CHECK(result.operation!=S_OK && durableBackend.writes==writes && !fs::exists(undoLedger/L"transaction.journal"));
    durableBackend.states[static_cast<size_t>(Resource::profile)]=original;
    auto failedUndo=undo;failedUndo.token="test-03";auto failedLedger=ledger.parent_path()/L"test-03";
    failedUndo.storage_root=failedLedger.wstring();fs::create_directories(failedLedger);
    durableBackend.fail=Resource::profile;
    result=msi_registration::rollback_committed_fresh(durableBackend,request,id,failedUndo);
    CHECK(result.operation==E_ACCESSDENIED && result.cleanup==S_OK);
    durableBackend.fail=Resource::count;
    CHECK(msi_registration::inspect_committed_fresh(durableBackend,id)==S_OK);
    durable_journal::Store failedJournal(failedLedger/L"transaction.journal",failedUndo,failedLedger);
    CHECK(failedJournal.read(failedUndo,&document)==S_OK && !document.committed);
    result=msi_registration::rollback_committed_fresh(durableBackend,request,id,undo);
    CHECK(result.operation==S_OK && result.cleanup==S_OK);
    for(const auto& state:durableBackend.states)CHECK(state==ResourceState{});
    durable_journal::Store undoJournal(undoLedger/L"transaction.journal",undo,undoLedger);
    CHECK(undoJournal.read(undo,&document)==S_OK && document.committed && document.all_operations_succeeded);
    ResourceStates removalSnapshot{};
    CHECK(msi_registration::inspect_committed_removal(durableBackend,id,undo,&removalSnapshot)==S_OK);
    auto restoration=id;restoration.token="test-04";auto restorationLedger=ledger.parent_path()/L"test-04";
    restoration.storage_root=restorationLedger.wstring();fs::create_directories(restorationLedger);
    durableBackend.states[static_cast<size_t>(Resource::com_x86)].present=true;
    result=msi_registration::restore_committed_removal(durableBackend,id,undo,restoration);
    CHECK(result.operation!=S_OK && !fs::exists(restorationLedger/L"transaction.journal"));
    durableBackend.states[static_cast<size_t>(Resource::com_x86)]=ResourceState{};
    auto failedRestore=id;failedRestore.token="test-05";auto failedRestoreLedger=ledger.parent_path()/L"test-05";
    failedRestore.storage_root=failedRestoreLedger.wstring();fs::create_directories(failedRestoreLedger);
    durableBackend.fail=Resource::profile;
    result=msi_registration::restore_committed_removal(durableBackend,id,undo,failedRestore);
    CHECK(result.operation==E_ACCESSDENIED && result.cleanup==S_OK);
    durableBackend.fail=Resource::count;
    for(const auto& state:durableBackend.states)CHECK(state==ResourceState{});
    result=msi_registration::restore_committed_removal(durableBackend,id,undo,restoration);
    CHECK(result.operation==S_OK && result.cleanup==S_OK && durableBackend.states==removalSnapshot);
    durable_journal::Store restorationJournal(restorationLedger/L"transaction.journal",restoration,restorationLedger);
    CHECK(restorationJournal.read(restoration,&document)==S_OK && document.committed && document.all_operations_succeeded);
    fs::remove(restorationLedger/L"transaction.journal");fs::remove(restorationLedger/L"transaction.snapshot");fs::remove(restorationLedger);
    fs::remove(failedRestoreLedger/L"transaction.journal");fs::remove(failedRestoreLedger/L"transaction.snapshot");fs::remove(failedRestoreLedger);
    fs::remove(failedLedger/L"transaction.journal");fs::remove(failedLedger/L"transaction.snapshot");fs::remove(failedLedger);
    fs::remove(undoLedger/L"transaction.journal");fs::remove(undoLedger/L"transaction.snapshot");fs::remove(undoLedger);
    fs::remove(ledger/L"transaction.journal");fs::remove(ledger/L"transaction.snapshot");fs::remove(ledger/L"transaction.final.snapshot");
    fs::remove(ledger);fs::remove(ledger.parent_path());fs::remove(ledger.parent_path().parent_path());fs::remove(sandbox);
    std::cout << "fresh_registration_tests=passed\n";
}
