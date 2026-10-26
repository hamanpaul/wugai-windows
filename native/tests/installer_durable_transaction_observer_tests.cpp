#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include "../installer/durable_transaction_observer.hpp"

#define CHECK(x) do {if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<"\n";return false;}}while(0)
namespace fs=std::filesystem;
using namespace boshiamy::installer;
namespace dj=boshiamy::installer::durable_journal;
namespace snap=boshiamy::installer::durable_snapshot;
namespace obs=boshiamy::installer::durable_observer;

struct FakeBackend final:Backend {
    ResourceStates states{};std::vector<Resource> applies,restores;
    std::array<unsigned,static_cast<size_t>(Resource::count)> reads{};
    std::function<void(Resource,unsigned,ResourceState*)> on_read;
    std::function<void(Resource)> on_apply;
    Resource fail_apply_resource=Resource::count;HRESULT fail_apply_status=E_ACCESSDENIED;
    Resource s_false_apply_resource=Resource::count;
    Resource s_false_restore_resource=Resource::count;
    Resource s_false_read_resource=Resource::count;unsigned s_false_read_number=0;
    HRESULT injected_read_status=S_FALSE;
    bool leave_s_false_output_untouched=false;
    bool fail_after_mutating=false,wrong_success_readback=false;
    HRESULT read(Resource resource,ResourceState* out)override {
        if(!out)return E_POINTER;
        const size_t i=static_cast<size_t>(resource);++reads[i];
        if(on_read)on_read(resource,reads[i],&states[i]);
        if(resource==s_false_read_resource&&reads[i]==s_false_read_number) {
            if(!leave_s_false_output_untouched)*out=states[i];
            return injected_read_status;
        }
        *out=states[i];return S_OK;
    }
    HRESULT apply(Resource resource,const ResourceState& desired)override {
        applies.push_back(resource);if(on_apply)on_apply(resource);
        if(resource==s_false_apply_resource)return S_FALSE;
        if(resource==fail_apply_resource) {
            if(fail_after_mutating)states[static_cast<size_t>(resource)]=desired;
            return fail_apply_status;
        }
        states[static_cast<size_t>(resource)]=desired;
        if(wrong_success_readback&&resource==Resource::com_x64)states[static_cast<size_t>(resource)].path=L"C:\\foreign\\other.dll";
        return S_OK;
    }
    HRESULT restore(Resource resource,const ResourceState& old)override {
        restores.push_back(resource);
        if(resource==s_false_restore_resource)return S_FALSE;
        states[static_cast<size_t>(resource)]=old;return S_OK;
    }
    HRESULT validate_install(const InstallRequest&)override{return S_OK;}
};
static InstallRequest request_for_test() {
    InstallRequest r;r.owner=L"LiuKai-TSF-v1";r.x64_binary_path=L"C:\\fixture\\x64\\BoshiamyTsf.dll";
    r.x86_binary_path=L"C:\\fixture\\x86\\BoshiamyTsf.dll";r.icon_path=L"C:\\fixture\\shared\\branding.dll";
    r.expected_x64_path=r.x64_binary_path;r.expected_x86_path=r.x86_binary_path;
    return r;
}
static dj::Identity identity_for(const fs::path& root) {
    dj::Identity id;id.owner="LiuKai-TSF-v1";id.token="observed-01";id.install_root=root.wstring();
    id.package_version="fixture-1";id.package_manifest_sha256=std::string(64,'a');return id;
}
static bool make_case(const fs::path& sandbox,const wchar_t* name,fs::path* root,
                      dj::Identity* id,dj::Store** journal,snap::Store** snapshot,
                      dj::FaultInjector* faults=nullptr) {
    std::error_code ec;*root=sandbox/name;
    if(!fs::create_directory(*root,ec)||ec)return false;
    *id=identity_for(*root);
    *journal=new dj::Store(*root/L".installer-journal",*id,faults);
    *snapshot=new snap::Store(*root/L".installer-prestate",*id);
    return true;
}
static bool test_install_records_prestate_and_ordered_observed_mutations(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* journal_ptr=nullptr;snap::Store* snapshot_ptr=nullptr;
    CHECK(make_case(sandbox,L"happy",&root,&id,&journal_ptr,&snapshot_ptr));
    auto& journal=*journal_ptr;auto& snapshot=*snapshot_ptr;
    obs::Observer observer(journal,snapshot,id);
    FakeBackend backend;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==S_OK&&result.cleanup==S_OK);
    const std::vector<Resource> expected_order={Resource::com_x64,Resource::com_x86,
        Resource::profile,Resource::keyboard_category,Resource::system_tray_category,
        Resource::ui_element_category};
    CHECK(backend.applies==expected_order);
    CHECK(backend.restores.empty());
    dj::Document document;CHECK(SUCCEEDED(journal.read(id,&document)));
    CHECK(!document.committed&&document.events.size()==14&&document.last_complete_sequence==7);
    CHECK(document.events[0].kind==dj::Event::Kind::intent&&document.events[0].intent.resource=="prestate"&&
          document.events[0].intent.action=="capture-install-or-repair");
    CHECK(document.events[1].kind==dj::Event::Kind::observation&&document.events[1].observation.operation_status==S_OK);
    const char* resources[]={"com-x64","com-x86","profile","keyboard-category","system-tray-category","ui-element-category"};
    for(size_t i=0;i<6;++i) {
        const auto& intent=document.events[2+i*2].intent;
        const auto& observation=document.events[3+i*2].observation;
        CHECK(document.events[2+i*2].kind==dj::Event::Kind::intent&&intent.resource==resources[i]&&intent.action=="apply");
        CHECK(document.events[3+i*2].kind==dj::Event::Kind::observation&&observation.sequence==intent.sequence&&
              observation.operation_status==S_OK&&observation.observed_fingerprint==intent.desired_fingerprint);
    }
    ResourceStates prestate{};std::string prestate_hash;
    CHECK(SUCCEEDED(snapshot.read(id,&prestate,&prestate_hash))&&prestate_hash==document.events[0].intent.desired_fingerprint);
    for(const auto& state:prestate)CHECK(!state.present);
    return true;
}
static bool test_post_observation_journal_failure_halts_without_rollback(const fs::path& sandbox) {
    bool fail=false;dj::FaultInjector faults{[&](dj::FaultPoint p){if(fail&&p==dj::FaultPoint::before_flush){fail=false;return true;}return false;}};
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"post-failure",&root,&id,&j,&s,&faults));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.on_apply=[&](Resource r){if(r==Resource::com_x64)fail=true;};
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(FAILED(result.operation)&&FAILED(result.cleanup));
    CHECK(backend.applies==std::vector<Resource>{Resource::com_x64}&&backend.restores.empty());
    CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].present&&observer.pending());
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events.back().kind==dj::Event::Kind::intent&&document.events.back().intent.resource=="com-x64");
    delete j;delete s;return true;
}
static bool test_precondition_foreign_change_is_preserved(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"foreign-precondition",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.on_read=[&](Resource r,unsigned n,ResourceState* state){
        if(r==Resource::com_x64&&n==2){state->present=true;state->owner=L"external";state->path=L"C:\\external\\tip.dll";state->threading_model=L"Apartment";}
    };
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(FAILED(result.operation)&&result.cleanup==S_OK&&backend.applies.empty()&&backend.restores.empty());
    CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].owner==L"external");
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&document.events.back().kind==dj::Event::Kind::observation);
    CHECK(document.events.back().observation.operation_status==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    delete j;delete s;return true;
}
static bool test_observer_pre_mutation_s_false_read_stops_without_writes(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-pre-read-s-false",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.s_false_read_resource=Resource::com_x64;backend.s_false_read_number=2;
    backend.leave_s_false_output_untouched=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    if(result.operation!=S_FALSE||result.cleanup!=S_FALSE)std::cerr<<"pre S_FALSE op="<<std::hex<<result.operation<<" cleanup="<<result.cleanup<<"\n";
    CHECK(result.operation==S_FALSE&&result.cleanup==S_FALSE);
    CHECK(backend.applies.empty()&&backend.restores.empty());
    CHECK(!backend.states[static_cast<size_t>(Resource::com_x64)].present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events.back().kind==dj::Event::Kind::observation);
    CHECK(document.events.back().observation.operation_status==S_FALSE&&
          document.events.back().observation.observed_fingerprint==obs::detail::unknown_fingerprint()&&
          !document.events.back().observation.mutation_may_have_occurred);
    delete j;delete s;return true;
}
static bool test_observer_post_mutation_s_false_read_records_unknown_without_rollback(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-post-read-s-false",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.s_false_read_resource=Resource::com_x64;backend.s_false_read_number=3;
    backend.leave_s_false_output_untouched=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==S_FALSE&&result.cleanup==S_FALSE);
    CHECK(backend.applies==std::vector<Resource>{Resource::com_x64}&&backend.restores.empty());
    CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events.back().kind==dj::Event::Kind::observation&&
          document.events.back().observation.operation_status==S_FALSE&&
          document.events.back().observation.observed_fingerprint==obs::detail::unknown_fingerprint()&&
          document.events.back().observation.mutation_may_have_occurred);
    delete j;delete s;return true;
}
static bool test_observer_pre_mutation_failed_read_does_not_claim_write(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr; snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-pre-read-failed-flag",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.s_false_read_resource=Resource::com_x64;backend.s_false_read_number=2;
    backend.injected_read_status=E_ACCESSDENIED;backend.leave_s_false_output_untouched=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==E_ACCESSDENIED&&result.cleanup==E_ACCESSDENIED);
    CHECK(backend.applies.empty()&&backend.restores.empty());
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    const auto& event=document.events.back();
    CHECK(event.kind==dj::Event::Kind::observation&&event.observation.operation_status==E_ACCESSDENIED&&
          event.observation.observed_fingerprint==obs::detail::unknown_fingerprint()&&
          !event.observation.mutation_may_have_occurred);
    delete j;delete s;return true;
}
static bool test_observer_post_mutation_failed_read_claims_possible_write(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr; snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-post-read-failed-flag",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.s_false_read_resource=Resource::com_x64;backend.s_false_read_number=3;
    backend.injected_read_status=E_ACCESSDENIED;backend.leave_s_false_output_untouched=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==E_ACCESSDENIED&&result.cleanup==E_ACCESSDENIED);
    CHECK(backend.applies==std::vector<Resource>{Resource::com_x64}&&backend.restores.empty());
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    const auto& event=document.events.back();
    CHECK(event.kind==dj::Event::Kind::observation&&event.observation.operation_status==E_ACCESSDENIED&&
          event.observation.observed_fingerprint==obs::detail::unknown_fingerprint()&&
          event.observation.mutation_may_have_occurred);
    delete j;delete s;return true;
}
static bool test_x64_shared_guard_s_false_is_preserved_and_rolls_back_com64(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"x64-guard-s-false",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    auto request=request_for_test();request.include_x86=false;request.expected_x86_path.clear();
    for(Resource resource:{Resource::com_x64,Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category}) {
        auto& state=backend.states[detail::index(resource)];state.present=true;state.created_by_lifecycle=true;
        state.owner=request.owner;
        if(resource==Resource::com_x64){state.path=request.expected_x64_path;state.threading_model=L"Apartment";}
        if(resource==Resource::profile)state.path=request.icon_path;
    }
    backend.s_false_read_resource=Resource::com_x86;backend.s_false_read_number=2;
    const auto result=uninstall(backend,request,observer);
    CHECK(result.operation==S_FALSE&&result.cleanup==S_OK);
    CHECK(backend.states[detail::index(Resource::com_x64)].present&&backend.restores==std::vector<Resource>{Resource::com_x64});
    CHECK(!backend.states[detail::index(Resource::com_x86)].present);
    CHECK(backend.states[detail::index(Resource::keyboard_category)].present&&backend.states[detail::index(Resource::system_tray_category)].present&&
          backend.states[detail::index(Resource::ui_element_category)].present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    bool saw_guard_s_false=false;
    for(size_t i=1;i<document.events.size();++i) {
        const auto& intent=document.events[i-1];const auto& observation=document.events[i];
        if(intent.kind==dj::Event::Kind::intent&&intent.intent.resource=="ui-element-category"&&
           intent.intent.action=="apply"&&observation.kind==dj::Event::Kind::observation) {
            saw_guard_s_false=observation.observation.operation_status==S_FALSE&&
                observation.observation.observed_fingerprint!=obs::detail::unknown_fingerprint()&&
                !observation.observation.mutation_may_have_occurred;
        }
    }
    CHECK(saw_guard_s_false);
    delete j;delete s;return true;
}
static bool test_observer_apply_s_false_without_write_is_recorded_and_stops(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-apply-s-false",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.s_false_apply_resource=Resource::com_x64;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==S_FALSE&&result.cleanup==S_OK);
    CHECK(backend.applies==std::vector<Resource>{Resource::com_x64}&&backend.restores.empty());
    CHECK(!backend.states[detail::index(Resource::com_x64)].present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    const auto& event=document.events.back();
    CHECK(event.kind==dj::Event::Kind::observation&&event.observation.operation_status==S_FALSE&&
          !event.observation.observed_fingerprint.empty()&&
          event.observation.observed_fingerprint!=obs::detail::unknown_fingerprint());
    delete j;delete s;return true;
}
static bool test_observer_restore_s_false_is_not_clean_cleanup(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"observer-restore-s-false",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.fail_apply_resource=Resource::com_x86;
    backend.s_false_restore_resource=Resource::com_x64;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==E_ACCESSDENIED&&result.cleanup==S_FALSE);
    CHECK(backend.states[detail::index(Resource::com_x64)].present);
    CHECK(backend.restores==std::vector<Resource>{Resource::com_x64});
    delete j;delete s;return true;
}
static bool test_success_with_wrong_readback_is_failed_and_preserved(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"wrong-readback",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;backend.wrong_success_readback=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    const HRESULT mismatch=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    CHECK(result.operation==mismatch&&result.cleanup==mismatch&&backend.restores.empty());
    CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].path==L"C:\\foreign\\other.dll");
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.all_operations_succeeded);
    CHECK(document.events.back().kind==dj::Event::Kind::observation&&document.events.back().observation.operation_status==mismatch);
    delete j;delete s;return true;
}
static bool test_second_com_failure_rolls_first_back_in_reverse(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"second-com-failure",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    backend.fail_apply_resource=Resource::com_x86;backend.fail_after_mutating=true;
    const auto result=install_or_repair(backend,request_for_test(),observer);
    CHECK(result.operation==E_ACCESSDENIED&&result.cleanup==S_OK);
    CHECK(backend.applies==std::vector<Resource>({Resource::com_x64,Resource::com_x86}));
    CHECK(backend.restores==std::vector<Resource>({Resource::com_x86,Resource::com_x64}));
    for(const auto& state:backend.states)CHECK(!state.present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events.size()==10);
    CHECK(document.events[6].kind==dj::Event::Kind::intent&&document.events[6].intent.action=="restore"&&document.events[6].intent.resource=="com-x86");
    CHECK(document.events[7].kind==dj::Event::Kind::observation&&document.events[7].observation.observed_fingerprint==document.events[6].intent.desired_fingerprint);
    CHECK(document.events[8].kind==dj::Event::Kind::intent&&document.events[8].intent.action=="restore"&&document.events[8].intent.resource=="com-x64");
    CHECK(document.events[9].kind==dj::Event::Kind::observation&&document.events[9].observation.observed_fingerprint==document.events[8].intent.desired_fingerprint);
    delete j;delete s;return true;
}
struct ArmRestoreFailure final:TransactionObserver {
    TransactionObserver& inner;bool& fail;
    ArmRestoreFailure(TransactionObserver& i,bool& f):inner(i),fail(f){}
    HRESULT begin(TransactionOperation o,const ResourceStates& p)override{return inner.begin(o,p);}
    HRESULT before(Resource r,TransactionMutation m,const ResourceState& e,const ResourceState& d)override{return inner.before(r,m,e,d);}
    HRESULT after(Resource r,TransactionMutation m,HRESULT op,HRESULT rb,const ResourceState* state,bool may)override {
        HRESULT result=inner.after(r,m,op,rb,state,may);
        if(SUCCEEDED(result)&&r==Resource::com_x86&&m==TransactionMutation::apply)fail=true;
        return result;
    }
};
static bool test_restore_intent_failure_stops_rollback(const fs::path& sandbox) {
    bool fail_flush=false;dj::FaultInjector faults{[&](dj::FaultPoint p){if(fail_flush&&p==dj::FaultPoint::before_flush){fail_flush=false;return true;}return false;}};
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"restore-intent-failure",&root,&id,&j,&s,&faults));
    obs::Observer observer(*j,*s,id);ArmRestoreFailure arm(observer,fail_flush);FakeBackend backend;
    backend.fail_apply_resource=Resource::com_x86;
    const auto result=install_or_repair(backend,request_for_test(),arm);
    CHECK(result.operation==E_ACCESSDENIED&&FAILED(result.cleanup));
    CHECK(backend.applies==std::vector<Resource>({Resource::com_x64,Resource::com_x86})&&backend.restores.empty());
    CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].present);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events.back().kind==dj::Event::Kind::observation&&document.events.back().observation.sequence==3);
    delete j;delete s;return true;
}
static bool test_uninstall_records_distinct_capture_action(const fs::path& sandbox) {
    fs::path root;dj::Identity id;dj::Store* j=nullptr;snap::Store* s=nullptr;
    CHECK(make_case(sandbox,L"uninstall-capture",&root,&id,&j,&s));
    obs::Observer observer(*j,*s,id);FakeBackend backend;
    const auto req=request_for_test();
    for(Resource r:{Resource::com_x64,Resource::com_x86,Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category}) {
        auto& state=backend.states[static_cast<size_t>(r)];state.present=true;state.owner=req.owner;state.created_by_lifecycle=true;
        if(r==Resource::com_x64){state.path=req.expected_x64_path;state.threading_model=L"Apartment";}
        if(r==Resource::com_x86){state.path=req.expected_x86_path;state.threading_model=L"Apartment";}
    }
    const auto result=uninstall(backend,req,observer);
    CHECK(result.operation==S_OK&&result.cleanup==S_OK);
    dj::Document document;CHECK(SUCCEEDED(j->read(id,&document))&&!document.committed);
    CHECK(document.events[0].kind==dj::Event::Kind::intent&&document.events[0].intent.resource=="prestate"&&
          document.events[0].intent.action=="capture-uninstall");
    for(const auto& state:backend.states)CHECK(!state.present);
    delete j;delete s;return true;
}

static dj::Identity machine_identity_for(const fs::path& root) {
    auto id=identity_for(root);id.schema_version=3;id.token=root.filename().string();
    id.storage_root=(root.parent_path()/dj::detail::ledger_namespace(id)/
        fs::path(id.owner)/fs::path(id.token)).wstring();
    return id;
}
static ResourceState machine_state_for_test() {
    ResourceState state;state.present=true;state.created_by_lifecycle=true;
    state.owner=L"LiuKai-TSF-v1";state.path=L"C:\\managed\\x64\\BoshiamyTsf.dll";
    state.threading_model=L"Apartment";state.private_data={1,0,3,0xff};return state;
}
static bool test_machine_fingerprints_bind_scope_resource_absence_and_identity(const fs::path& sandbox) {
    const auto id=machine_identity_for(sandbox/L"machine-fingerprints");
    CHECK(dj::detail::valid_identity(id));
    const auto state=machine_state_for_test();
    std::string present,absent,user_absent,other_binding,exact;
    CHECK(obs::detail::fingerprint_state(id,Resource::machine_com_x64,state,&present,false));
    CHECK(obs::detail::fingerprint_state(id,Resource::machine_com_x64,{},&absent,false));
    CHECK(obs::detail::fingerprint_state(id,Resource::com_x64,{},&user_absent,false));
    CHECK(present!=absent&&absent!=user_absent);
    auto changed=state;changed.private_data.back()^=1;
    CHECK(obs::detail::fingerprint_state(id,Resource::machine_com_x64,changed,&other_binding,false));
    CHECK(present!=other_binding);
    CHECK(obs::detail::fingerprint_state(id,Resource::machine_com_x64,state,&exact,true));
    CHECK(present==exact); // machine private data is never silently discarded
    const auto legacy_id=identity_for(sandbox/L"machine-fingerprints");
    CHECK(!obs::detail::fingerprint_state(legacy_id,Resource::machine_com_x64,{},&absent,false));
    std::string legacy_user;
    CHECK(obs::detail::fingerprint_state(legacy_id,Resource::com_x64,{},&legacy_user,false));
    CHECK(legacy_user!=user_absent);
    fs::create_directories(legacy_id.install_root);
    dj::Store legacy_journal(fs::path(legacy_id.install_root)/L"legacy.journal",legacy_id);
    CHECK(legacy_journal.create()==S_OK);
    uint64_t sequence=99;
    CHECK(legacy_journal.append_intent("com-machine-x64","apply",std::string(64,'a'),
        std::string(64,'b'),&sequence)==E_INVALIDARG&&sequence==0);
    dj::Document legacy_document;CHECK(legacy_journal.read(legacy_id,&legacy_document)==S_OK&&legacy_document.events.empty());
    return true;
}
static bool test_machine_journal_restores_exact_absence_and_commits(const fs::path& sandbox) {
    auto id=machine_identity_for(sandbox/L"machine-journal");
    fs::create_directories(id.storage_root);
    const auto ledger=fs::path(id.storage_root);
    dj::Store journal(ledger/L"transaction.journal",id);
    snap::Store snapshot(ledger/L"transaction.snapshot",id);
    obs::Observer observer(journal,snapshot,id);
    ResourceStates pre{};const auto desired=machine_state_for_test();
    CHECK(observer.begin(TransactionOperation::install_or_repair,pre)==S_OK);
    CHECK(observer.before(Resource::machine_com_x64,TransactionMutation::apply,{},desired)==S_OK);
    CHECK(observer.after(Resource::machine_com_x64,TransactionMutation::apply,S_OK,S_OK,&desired,true)==S_OK);
    CHECK(observer.before(Resource::machine_com_x64,TransactionMutation::restore,desired,{})==S_OK);
    const ResourceState absent{};
    CHECK(observer.after(Resource::machine_com_x64,TransactionMutation::restore,S_OK,S_OK,&absent,true)==S_OK);
    CHECK(observer.commit(pre)==S_OK);
    dj::Document document;CHECK(journal.read(id,&document)==S_OK&&document.committed);
    CHECK(document.events.size()==7&&document.events[2].intent.resource=="com-machine-x64"&&
        document.events[4].intent.resource=="com-machine-x64"&&document.events[4].intent.action=="restore");
    CHECK(document.events[4].intent.desired_fingerprint==document.events[5].observation.observed_fingerprint);
    ResourceStates captured{};CHECK(snapshot.read(id,&captured)==S_OK&&captured==pre);
    auto old_scope=id;old_scope.schema_version=2;
    old_scope.storage_root=(fs::path(id.install_root).parent_path()/L".liukai-installer-ledger"/
        fs::path(id.owner)/fs::path(id.token)).wstring();
    CHECK(journal.read(old_scope,&document)==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    return true;
}
static bool test_unclosed_machine_intent_preserves_journal_and_blocks_more_mutation(const fs::path& sandbox) {
    auto id=machine_identity_for(sandbox/L"machine-unclosed");
    fs::create_directories(id.storage_root);const auto ledger=fs::path(id.storage_root);
    bool fail=false;dj::FaultInjector faults{[&](dj::FaultPoint point){return fail&&point==dj::FaultPoint::before_flush;}};
    dj::Store journal(ledger/L"transaction.journal",id,&faults);
    snap::Store snapshot(ledger/L"transaction.snapshot",id);
    obs::Observer observer(journal,snapshot,id);
    ResourceStates pre{};const auto desired=machine_state_for_test();
    CHECK(observer.begin(TransactionOperation::install_or_repair,pre)==S_OK);
    CHECK(observer.before(Resource::machine_com_x64,TransactionMutation::apply,{},desired)==S_OK);
    fail=true;
    CHECK(FAILED(observer.after(Resource::machine_com_x64,TransactionMutation::apply,S_OK,S_OK,&desired,true)));
    CHECK(observer.pending());
    CHECK(FAILED(observer.before(Resource::machine_com_x64,TransactionMutation::restore,desired,{})));
    auto final=pre;final[6]=desired;CHECK(FAILED(observer.commit(final)));
    fail=false;
    dj::Document document;CHECK(journal.read(id,&document)==S_OK&&!document.committed);
    CHECK(document.events.size()==3&&document.events.back().kind==dj::Event::Kind::intent&&
        document.events.back().intent.resource=="com-machine-x64");
    // A new observer cannot overwrite the sidecar or close a prior observer's intent.
    obs::Observer reopened(journal,snapshot,id);
    CHECK(FAILED(reopened.begin(TransactionOperation::install_or_repair,pre)));
    CHECK(FAILED(reopened.before(Resource::machine_com_x64,TransactionMutation::restore,desired,{})));
    CHECK(journal.read(id,&document)==S_OK&&document.events.size()==3);
    ResourceStates captured{};CHECK(snapshot.read(id,&captured)==S_OK&&captured==pre);
    return true;
}

int wmain(int argc,wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    if(argc!=2||!fs::path(argv[1]).is_absolute()) {
        std::wcerr<<L"usage: installer-durable-transaction-observer-tests.exe <absolute-new-sandbox>\n";return 2;
    }
    const fs::path sandbox=argv[1];std::error_code ec;
    if(fs::exists(sandbox,ec)||ec||!fs::create_directory(sandbox,ec)||ec)return 2;
    if(!test_machine_fingerprints_bind_scope_resource_absence_and_identity(sandbox))return 1;
    if(!test_machine_journal_restores_exact_absence_and_commits(sandbox))return 1;
    if(!test_unclosed_machine_intent_preserves_journal_and_blocks_more_mutation(sandbox))return 1;
    if(!test_install_records_prestate_and_ordered_observed_mutations(sandbox))return 1;
    if(!test_post_observation_journal_failure_halts_without_rollback(sandbox))return 1;
    if(!test_precondition_foreign_change_is_preserved(sandbox))return 1;
    if(!test_observer_pre_mutation_s_false_read_stops_without_writes(sandbox))return 1;
    if(!test_observer_post_mutation_s_false_read_records_unknown_without_rollback(sandbox))return 1;
    if(!test_observer_pre_mutation_failed_read_does_not_claim_write(sandbox))return 1;
    if(!test_observer_post_mutation_failed_read_claims_possible_write(sandbox))return 1;
    if(!test_x64_shared_guard_s_false_is_preserved_and_rolls_back_com64(sandbox))return 1;
    if(!test_observer_apply_s_false_without_write_is_recorded_and_stops(sandbox))return 1;
    if(!test_observer_restore_s_false_is_not_clean_cleanup(sandbox))return 1;
    if(!test_success_with_wrong_readback_is_failed_and_preserved(sandbox))return 1;
    if(!test_second_com_failure_rolls_first_back_in_reverse(sandbox))return 1;
    if(!test_restore_intent_failure_stops_rollback(sandbox))return 1;
    if(!test_uninstall_records_distinct_capture_action(sandbox))return 1;
    std::cout<<"durable transaction observer tests passed; fake backend only\n";return 0;
}
