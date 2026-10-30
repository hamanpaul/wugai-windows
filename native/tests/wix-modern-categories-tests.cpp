#include "../wix/modern-categories.hpp"
#include <iostream>
namespace c=boshiamy::installer::package_categories;
namespace m=boshiamy::installer::msi_modern_categories;
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<'\n';return 1;}}while(0)
struct Memory final:c::Store {
    c::Categories state{GUID_TFCAT_TIP_KEYBOARD,c::tray,c::ui};int writes=0,failWrite=0;
    HRESULT read(c::Categories* out) override {*out=state;return S_OK;}
    HRESULT add(const GUID& id) override {if(++writes==failWrite)return E_ACCESSDENIED;state.push_back(id);return S_OK;}
    HRESULT remove(const GUID& id) override {if(++writes==failWrite)return E_ACCESSDENIED;state.erase(std::remove_if(state.begin(),state.end(),[&](const GUID& x){return IsEqualGUID(x,id);}),state.end());return S_OK;}
};
struct Log final:m::Observer {
    int started=0,intents=0,observations=0,committed=0;bool refuseIntent=false,refuseObservation=false;
    HRESULT begin(const c::Categories&,const c::Categories&) override {++started;return S_OK;}
    HRESULT before(const GUID&,bool,const c::Categories&,const c::Categories&) override {++intents;return refuseIntent?E_FAIL:S_OK;}
    HRESULT after(const GUID&,bool,HRESULT,HRESULT,const c::Categories*) override {++observations;return refuseObservation?E_FAIL:S_OK;}
    HRESULT commit(const c::Categories&) override {++committed;return S_OK;}
};
int main(){
    Memory store;Log log;auto baseline=store.state;
    CHECK(m::transition(store,baseline,true,log)==S_OK&&store.writes==2&&log.committed==1);
    auto enabled=store.state;Log remove;
    CHECK(m::transition(store,enabled,false,remove)==S_OK&&c::same(store.state,baseline));
    Log conflict;CHECK(m::transition(store,enabled,true,conflict)!=S_OK&&conflict.started==0);
    Memory bad;Log noIntent;noIntent.refuseIntent=true;
    CHECK(m::transition(bad,bad.state,true,noIntent)==E_FAIL&&bad.writes==0);
    Memory uncertain;Log noObservation;noObservation.refuseObservation=true;
    CHECK(m::transition(uncertain,uncertain.state,true,noObservation)==E_FAIL&&uncertain.writes==1&&noObservation.committed==0);
    Memory failing;Log failure;failing.failWrite=2;
    CHECK(m::transition(failing,failing.state,true,failure)==E_ACCESSDENIED&&failing.writes==2&&failure.observations==2&&failure.committed==0);
    namespace fs=std::filesystem;namespace j=boshiamy::installer::durable_journal;
    auto root=fs::absolute(fs::path(L"build")/L"tests"/(L"modern-journal-"+std::to_wstring(GetCurrentProcessId())));fs::create_directories(root);
    j::Identity id;id.owner="LiuKai-TSF-v1";id.token="test-modern";id.install_root=root.wstring();id.package_version="test";id.package_manifest_sha256=std::string(64,'a');
    j::Store durable(root/L"modern.journal",id);m::DurableObserver durableLog(durable,id);Memory verified;
    auto initial=verified.state;CHECK(m::transition(verified,initial,true,durableLog)==S_OK);
    j::Document document;CHECK(durable.read(id,&document)==S_OK&&document.committed&&document.all_operations_succeeded&&document.events.size()==7);
    CHECK(m::inspect_committed(verified,durable,id,true)==S_OK);
    verified.state.erase(verified.state.begin());
    CHECK(m::inspect_committed(verified,durable,id,true)!=S_OK);
    verified.state.push_back(GUID_TFCAT_TIP_KEYBOARD);
    CHECK(m::inspect_committed(verified,durable,id,true)==S_OK);
    j::Store removal(root/L"removal.journal",id);m::DurableObserver removalLog(removal,id);auto full=verified.state;
    CHECK(m::transition(verified,full,false,removalLog)==S_OK);
    CHECK(m::inspect_committed(verified,removal,id,false)==S_OK);
    CHECK(m::inspect_committed(verified,durable,id,true)!=S_OK);
    m::DurableObserver duplicate(durable,id);Memory duplicateStore;
    CHECK(m::transition(duplicateStore,duplicateStore.state,true,duplicate)!=S_OK&&duplicateStore.writes==0);
    j::Store failedStore(root/L"failed.journal",id);m::DurableObserver failedLog(failedStore,id);Memory partial;partial.failWrite=2;
    CHECK(m::transition(partial,partial.state,true,failedLog)==E_ACCESSDENIED);
    CHECK(failedStore.read(id,&document)==S_OK&&!document.committed&&!document.all_operations_succeeded&&document.events.size()==6);
    CHECK(c::contains(partial.state,c::immersive)&&!c::contains(partial.state,c::input_mode));
    j::Store recoverStore(root/L"recover.journal",id);m::DurableObserver recovery(recoverStore,id);
    partial.state.push_back(c::input_mode);
    CHECK(m::recover_partial(partial,failedStore,id,true,recovery)!=S_OK&&!fs::exists(root/L"recover.journal"));
    partial.state.erase(std::remove_if(partial.state.begin(),partial.state.end(),[](const GUID& x){return IsEqualGUID(x,c::input_mode);}),partial.state.end());
    partial.failWrite=0;
    CHECK(m::recover_partial(partial,failedStore,id,true,recovery)==S_OK&&c::same(partial.state,baseline));
    CHECK(recoverStore.read(id,&document)==S_OK&&document.committed&&document.all_operations_succeeded);
    j::Store pendingStore(root/L"pending.journal",id);m::DurableObserver pending(pendingStore,id);
    c::Categories target=baseline;target.push_back(c::immersive);target.push_back(c::input_mode);
    c::Categories first=baseline;first.push_back(c::immersive);
    CHECK(pending.begin(baseline,target)==S_OK&&pending.before(c::immersive,true,baseline,first)==S_OK);
    Memory unknown;unknown.state=first;Log noRecovery;
    CHECK(m::recover_partial(unknown,pendingStore,id,true,noRecovery)!=S_OK&&noRecovery.started==0&&unknown.writes==0);
    j::Store failedRemoval(root/L"failed-removal.journal",id);m::DurableObserver failedRemovalLog(failedRemoval,id);
    Memory removalPartial;removalPartial.state=target;removalPartial.failWrite=2;
    CHECK(m::transition(removalPartial,target,false,failedRemovalLog)==E_ACCESSDENIED);
    j::Store restoreRemoval(root/L"restore-removal.journal",id);m::DurableObserver restoreRemovalLog(restoreRemoval,id);
    removalPartial.failWrite=0;
    CHECK(m::recover_partial(removalPartial,failedRemoval,id,false,restoreRemovalLog)==S_OK&&c::same(removalPartial.state,target));
    fs::remove(root/L"modern.journal");fs::remove(root/L"removal.journal");fs::remove(root/L"failed.journal");fs::remove(root/L"recover.journal");fs::remove(root/L"pending.journal");fs::remove(root/L"failed-removal.journal");fs::remove(root/L"restore-removal.journal");fs::remove(root);
    std::cout<<"modern_category_transaction_tests=passed\n";
}
