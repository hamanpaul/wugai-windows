#include <windows.h>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "../installer/dual_architecture_transaction.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return 1; } } while (0)

using namespace boshiamy::installer;

struct FakeBackend final : Backend {
    std::vector<ResourceState> state;
    std::vector<std::string> events;
    Resource fail_apply = Resource::count;
    Resource fail_restore = Resource::count;
    bool mutate_before_failure = false;
    HRESULT health_result = S_OK;
    unsigned health_reads = 0;
    FakeBackend() : state(static_cast<size_t>(Resource::count)) {}
    HRESULT read(Resource resource, ResourceState* out) override {
        if (!out) return E_POINTER;
        *out = state[static_cast<size_t>(resource)];
        events.push_back("read:" + std::to_string(static_cast<int>(resource)));
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        events.push_back("apply:" + std::to_string(static_cast<int>(resource)));
        if (resource == fail_apply) {
            if (mutate_before_failure) state[static_cast<size_t>(resource)] = desired;
            return E_ACCESSDENIED;
        }
        state[static_cast<size_t>(resource)] = desired;
        return S_OK;
    }
    HRESULT restore(Resource resource, const ResourceState& prior) override {
        events.push_back("restore:" + std::to_string(static_cast<int>(resource)));
        if (resource == fail_restore) return E_FAIL;
        state[static_cast<size_t>(resource)] = prior;
        return S_OK;
    }
    HRESULT verify_install_health() override { ++health_reads; return health_result; }
};

struct RecordingObserver final : TransactionObserver {
    unsigned begins = 0, commits = 0;
    std::vector<std::pair<Resource, TransactionMutation>> before_events, after_events;
    HRESULT begin(TransactionOperation, const ResourceStates&) override { ++begins; return S_OK; }
    HRESULT before(Resource resource, TransactionMutation mutation,
                   const ResourceState&, const ResourceState&) override {
        before_events.emplace_back(resource, mutation); return S_OK;
    }
    HRESULT after(Resource resource, TransactionMutation mutation, HRESULT operation_status,
                  HRESULT readback_status, const ResourceState*, bool) override {
        if (FAILED(operation_status)) return operation_status;
        if (FAILED(readback_status)) return readback_status;
        after_events.emplace_back(resource, mutation); return S_OK;
    }
    HRESULT commit(const ResourceStates&) override { ++commits; return S_OK; }
};

int main() {
    FakeBackend backend;
    InstallRequest request{L"LiuKai-TSF-v1", L"C:\\LiuKai\\x64\\r2\\BoshiamyTsf.dll",
                           L"C:\\LiuKai\\x86\\r2\\BoshiamyTsf.dll",
                           L"C:\\LiuKai\\shared\\Boshiamy.ico", L"C:\\LiuKai\\x86\\r1\\BoshiamyTsf.dll",
                           L"C:\\LiuKai\\x64\\r1\\BoshiamyTsf.dll"};
    backend.fail_apply = Resource::com_x86;
    auto failed = install_or_repair(backend, request);
    CHECK(failed.operation == E_ACCESSDENIED);
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)].present == false);
    CHECK(backend.events.size() > 0);
    CHECK(backend.events[backend.events.size()-1] == "restore:0");

    FakeBackend preserve;
    auto& profile = preserve.state[static_cast<size_t>(Resource::profile)];
    profile.present = true; profile.owner = request.owner; profile.path = L"C:\\old\\Boshiamy.ico";
    profile.created_by_lifecycle = false; profile.private_data = {1,2,3,4};
    auto& category = preserve.state[static_cast<size_t>(Resource::keyboard_category)];
    category.present = true; category.owner = request.owner; category.created_by_lifecycle = false;
    auto& tray_category = preserve.state[static_cast<size_t>(Resource::system_tray_category)];
    tray_category.present = true; tray_category.owner = request.owner; tray_category.created_by_lifecycle = false;
    request.expected_x86_path.clear(); request.expected_x64_path.clear();
    auto installed = install_or_repair(preserve, request);
    CHECK(SUCCEEDED(installed.operation));
    CHECK(preserve.state[static_cast<size_t>(Resource::profile)].present);
    CHECK(!preserve.state[static_cast<size_t>(Resource::profile)].created_by_lifecycle);
    CHECK(preserve.state[static_cast<size_t>(Resource::profile)].private_data == std::vector<unsigned char>({1,2,3,4}));
    request.expected_x86_path=request.x86_binary_path; request.expected_x64_path=request.x64_binary_path;
    auto borrowed_uninstall=uninstall(preserve,request);
    CHECK(SUCCEEDED(borrowed_uninstall.operation));
    CHECK(preserve.state[static_cast<size_t>(Resource::profile)].present);
    CHECK(preserve.state[static_cast<size_t>(Resource::keyboard_category)].present);
    CHECK(preserve.state[static_cast<size_t>(Resource::system_tray_category)].present);
    CHECK(preserve.state[static_cast<size_t>(Resource::profile)].private_data == std::vector<unsigned char>({1,2,3,4}));

    FakeBackend repair;
    auto& x64 = repair.state[static_cast<size_t>(Resource::com_x64)];
    x64.present=true; x64.owner=request.owner; x64.path=L"C:\\old\\BoshiamyTsf.dll"; x64.threading_model=L"Apartment";
    InstallRequest repair_request{request.owner, request.x64_binary_path, request.x86_binary_path, request.icon_path,
                                  L"", L"C:\\old\\BoshiamyTsf.dll"};
    auto repaired=install_or_repair(repair,repair_request);
    CHECK(SUCCEEDED(repaired.operation));
    CHECK(repair.state[static_cast<size_t>(Resource::com_x86)].present);
    CHECK(repair.state[static_cast<size_t>(Resource::com_x64)].path==request.x64_binary_path);
    CHECK(repair.state[static_cast<size_t>(Resource::com_x86)].path==request.x86_binary_path);

    // An unhealthy idempotent repair still begins durable observation and
    // checks platform health, without creating a mutation or a commit.
    repair.events.clear();
    repair.health_result=HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    RecordingObserver no_op_observer;
    auto unhealthy_no_op=install_or_repair(repair,repair_request,&no_op_observer);
    CHECK(unhealthy_no_op.operation==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(no_op_observer.begins==1 && no_op_observer.commits==0);
    CHECK(no_op_observer.before_events.empty() && no_op_observer.after_events.empty());
    CHECK(repair.health_reads==2);
    for(const auto& event:repair.events) CHECK(event.rfind("apply:",0)!=0 && event.rfind("restore:",0)!=0);

    // A health failure after COM changes rolls the touched resources back in
    // reverse order and records each restore through the observer.
    repair.health_result=E_ACCESSDENIED;
    auto changed_repair_request=repair_request;
    changed_repair_request.x64_binary_path=L"C:\\LiuKai\\x64\\r3\\BoshiamyTsf.dll";
    changed_repair_request.x86_binary_path=L"C:\\LiuKai\\x86\\r3\\BoshiamyTsf.dll";
    changed_repair_request.expected_x64_path=repair_request.x64_binary_path;
    changed_repair_request.expected_x86_path=repair_request.x86_binary_path;
    RecordingObserver rollback_observer;
    auto health_rollback=install_or_repair(repair,changed_repair_request,&rollback_observer);
    CHECK(health_rollback.operation==E_ACCESSDENIED && health_rollback.cleanup==S_OK);
    CHECK(repair.state[static_cast<size_t>(Resource::com_x64)].path==repair_request.x64_binary_path);
    CHECK(repair.state[static_cast<size_t>(Resource::com_x86)].path==repair_request.x86_binary_path);
    CHECK(rollback_observer.begins==1 && rollback_observer.commits==0);
    CHECK(rollback_observer.before_events.size()==4);
    CHECK(rollback_observer.before_events[0]==std::make_pair(Resource::com_x64,TransactionMutation::apply));
    CHECK(rollback_observer.before_events[1]==std::make_pair(Resource::com_x86,TransactionMutation::apply));
    CHECK(rollback_observer.before_events[2]==std::make_pair(Resource::com_x86,TransactionMutation::restore));
    CHECK(rollback_observer.before_events[3]==std::make_pair(Resource::com_x64,TransactionMutation::restore));

    // Uninstall must remain available when install health is already bad.
    const unsigned health_reads_before_uninstall=repair.health_reads;
    auto uninstall_request=repair_request;
    uninstall_request.expected_x64_path=repair_request.x64_binary_path;
    uninstall_request.expected_x86_path=repair_request.x86_binary_path;
    auto health_bad_uninstall=uninstall(repair,uninstall_request);
    CHECK(health_bad_uninstall.operation==S_OK);
    CHECK(repair.health_reads==health_reads_before_uninstall);

    FakeBackend conflict;
    auto& conflict_com=conflict.state[static_cast<size_t>(Resource::com_x86)];
    conflict_com.present=true; conflict_com.owner=L"Other"; conflict_com.path=L"C:\\other\\other.dll";
    auto conflict_result=install_or_repair(conflict,request);
    CHECK(conflict_result.operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(conflict.events.size()==static_cast<size_t>(Resource::count));
    FakeBackend shared_conflict;
    auto& foreign_profile=shared_conflict.state[static_cast<size_t>(Resource::profile)];
    foreign_profile.present=true; foreign_profile.owner=L"Other";
    CHECK(install_or_repair(shared_conflict,request).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    FakeBackend path_conflict;
    request.expected_x64_path=L"C:\\LiuKai\\x64\\r1\\BoshiamyTsf.dll";
    auto& wrong_path=path_conflict.state[static_cast<size_t>(Resource::com_x64)];
    wrong_path.present=true; wrong_path.owner=request.owner; wrong_path.path=L"C:\\other\\old.dll"; wrong_path.threading_model=L"Apartment";
    CHECK(install_or_repair(path_conflict,request).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));

    FakeBackend stale_uninstall;
    auto& stale_x64=stale_uninstall.state[static_cast<size_t>(Resource::com_x64)];
    stale_x64.present=true; stale_x64.owner=request.owner; stale_x64.path=request.x64_binary_path; stale_x64.threading_model=L"Apartment";
    auto& stale_x86=stale_uninstall.state[static_cast<size_t>(Resource::com_x86)];
    stale_x86.present=true; stale_x86.owner=request.owner; stale_x86.path=request.x86_binary_path; stale_x86.threading_model=L"Apartment";
    auto stale_request=request; stale_request.expected_x64_path=L"C:\\LiuKai\\x64\\stale.dll";
    stale_request.expected_x86_path=request.x86_binary_path;
    auto stale_result=uninstall(stale_uninstall,stale_request);
    CHECK(stale_result.operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(stale_uninstall.events.size()==static_cast<size_t>(Resource::count));
    CHECK(stale_x64.present && stale_x86.present);

    FakeBackend foreign_uninstall;
    auto& foreign_x64=foreign_uninstall.state[static_cast<size_t>(Resource::com_x64)];
    foreign_x64.present=true; foreign_x64.owner=L"Other"; foreign_x64.path=request.x64_binary_path; foreign_x64.threading_model=L"Apartment";
    auto& foreign_x86=foreign_uninstall.state[static_cast<size_t>(Resource::com_x86)];
    foreign_x86.present=true; foreign_x86.owner=request.owner; foreign_x86.path=request.x86_binary_path; foreign_x86.threading_model=L"Apartment";
    auto foreign_result=uninstall(foreign_uninstall,request);
    CHECK(foreign_result.operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(foreign_uninstall.events.size()==static_cast<size_t>(Resource::count));
    CHECK(foreign_x64.present && foreign_x86.present);

    FakeBackend uninstall_backend;
    request.expected_x86_path=request.x86_binary_path; request.expected_x64_path=request.x64_binary_path;
    CHECK(SUCCEEDED(install_or_repair(uninstall_backend,request).operation));
    uninstall_backend.fail_apply=Resource::profile;
    auto removed=uninstall(uninstall_backend,request);
    CHECK(FAILED(removed.operation));
    CHECK(uninstall_backend.state[static_cast<size_t>(Resource::com_x86)].present);
    CHECK(uninstall_backend.state[static_cast<size_t>(Resource::com_x64)].present);
    CHECK(removed.cleanup==S_OK);
    FakeBackend clean_uninstall;
    CHECK(SUCCEEDED(install_or_repair(clean_uninstall,request).operation));
    auto complete_remove=uninstall(clean_uninstall,request);
    CHECK(SUCCEEDED(complete_remove.operation));
    CHECK(!clean_uninstall.state[static_cast<size_t>(Resource::com_x86)].present);
    CHECK(!clean_uninstall.state[static_cast<size_t>(Resource::com_x64)].present);
    CHECK(!clean_uninstall.state[static_cast<size_t>(Resource::profile)].present);
    CHECK(!clean_uninstall.state[static_cast<size_t>(Resource::keyboard_category)].present);
    CHECK(!clean_uninstall.state[static_cast<size_t>(Resource::system_tray_category)].present);
    // Slot meanings remain stable. Machine policy is explicit and x64-only.
    CHECK(static_cast<size_t>(Resource::com_x64)==0);
    CHECK(static_cast<size_t>(Resource::ui_element_category)==5);
    CHECK(static_cast<size_t>(Resource::machine_com_x64)==6);
    InstallRequest machine_request{request.owner, request.x64_binary_path, L"", request.icon_path,
                                   L"", request.expected_x64_path};
    CHECK(!machine_request.include_machine_com_x64);
    machine_request.include_x86=false;
    machine_request.include_machine_com_x64=true;
    const size_t user_slot=static_cast<size_t>(Resource::com_x64);
    const size_t machine_slot=static_cast<size_t>(Resource::machine_com_x64);
    const size_t x86_slot=static_cast<size_t>(Resource::com_x86);
    FakeBackend migrated;
    migrated.state[user_slot]={true,true,request.owner,request.x64_binary_path,L"Apartment",{1,2,3}};
    migrated.state[x86_slot]={true,false,L"foreign-x86",L"C:\\foreign\\x86.dll",L"Apartment",{9,8}};
    for(Resource resource:{Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category})
        migrated.state[static_cast<size_t>(resource)]={true,false,request.owner,request.icon_path,L"",{4,5}};
    const auto migration_before=migrated.state;
    auto migration=install_or_repair(migrated,machine_request);
    CHECK(migration.operation==S_OK && migration.cleanup==S_OK);
    CHECK(migrated.state[user_slot]==migration_before[user_slot]);
    CHECK(migrated.state[machine_slot].present && migrated.state[machine_slot].created_by_lifecycle);
    CHECK(migrated.state[machine_slot].path==machine_request.x64_binary_path);
    CHECK(migrated.state[x86_slot]==migration_before[x86_slot]);
    for(Resource resource:{Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category})
        CHECK(migrated.state[static_cast<size_t>(resource)]==migration_before[static_cast<size_t>(resource)]);
    migrated.events.clear();
    CHECK(install_or_repair(migrated,machine_request).operation==S_OK);
    for(const auto& event:migrated.events)CHECK(event.rfind("apply:",0)!=0 && event.rfind("restore:",0)!=0);

    // Legacy callers cannot modify a visible managed machine pair.
    auto legacy_request=machine_request;legacy_request.include_machine_com_x64=false;
    migrated.events.clear();
    CHECK(install_or_repair(migrated,legacy_request).operation==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    CHECK(uninstall(migrated,legacy_request).operation==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
    for(const auto& event:migrated.events)CHECK(event.rfind("apply:",0)!=0 && event.rfind("restore:",0)!=0);

    const auto paired_before=migrated.state;
    auto paired_upgrade=machine_request;
    paired_upgrade.expected_x64_path=machine_request.x64_binary_path;
    paired_upgrade.x64_binary_path=L"C:\\LiuKai\\x64\\r4\\BoshiamyTsf.dll";
    // A partial machine write compensates both exact trees, including opaque bytes.
    migrated.fail_apply=Resource::machine_com_x64;migrated.mutate_before_failure=true;
    auto pair_failure=install_or_repair(migrated,paired_upgrade);
    CHECK(pair_failure.operation==E_ACCESSDENIED && pair_failure.cleanup==S_OK);
    CHECK(migrated.state==paired_before);
    migrated.fail_restore=Resource::com_x64;
    auto pair_cleanup_failure=install_or_repair(migrated,paired_upgrade);
    CHECK(pair_cleanup_failure.operation==E_ACCESSDENIED && pair_cleanup_failure.cleanup==E_FAIL);
    migrated.state=paired_before;migrated.fail_restore=Resource::count;
    migrated.fail_apply=Resource::count;migrated.mutate_before_failure=false;
    CHECK(install_or_repair(migrated,paired_upgrade).operation==S_OK);
    CHECK(migrated.state[user_slot].path==paired_upgrade.x64_binary_path);
    CHECK(migrated.state[machine_slot].path==paired_upgrade.x64_binary_path);
    CHECK(migrated.state[user_slot].private_data==paired_before[user_slot].private_data);

    // Refuse foreign/untracked, wrong-threading, and stale machine registrations
    // before either x64 resource is written, even if HKCU is valid.
    for(unsigned conflict_kind=0;conflict_kind<4;++conflict_kind) {
        FakeBackend foreign_machine=migrated;
        auto& foreign=foreign_machine.state[machine_slot];
        if(conflict_kind==0)foreign.owner=L"Other";
        if(conflict_kind==1)foreign.created_by_lifecycle=false;
        if(conflict_kind==2)foreign.threading_model=L"Both";
        if(conflict_kind==3)foreign.path=L"C:\\other\\BoshiamyTsf.dll";
        foreign_machine.events.clear();
        const auto before_conflict=foreign_machine.state;
        CHECK(install_or_repair(foreign_machine,paired_upgrade).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        CHECK(foreign_machine.state==before_conflict);
        for(const auto& event:foreign_machine.events)CHECK(event.rfind("apply:",0)!=0 && event.rfind("restore:",0)!=0);
    }
    auto machine_uninstall=paired_upgrade;
    machine_uninstall.expected_x64_path=machine_request.x64_binary_path;
    migrated.events.clear();
    const auto before_stale=migrated.state;
    CHECK(uninstall(migrated,machine_uninstall).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(migrated.state==before_stale);
    machine_uninstall.expected_x64_path=paired_upgrade.x64_binary_path;
    migrated.health_result=E_ACCESSDENIED;
    const unsigned health_before_remove=migrated.health_reads;
    CHECK(uninstall(migrated,machine_uninstall).operation==S_OK);
    CHECK(!migrated.state[user_slot].present && !migrated.state[machine_slot].present);
    CHECK(migrated.health_reads==health_before_remove);
    CHECK(migrated.state[x86_slot]==migration_before[x86_slot]);
    for(Resource resource:{Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category})
        CHECK(migrated.state[static_cast<size_t>(resource)]==migration_before[static_cast<size_t>(resource)]);

    FakeBackend fresh_machine;
    CHECK(install_or_repair(fresh_machine,machine_request).operation==S_OK);
    CHECK(fresh_machine.state[user_slot].present && fresh_machine.state[machine_slot].present);
    auto fresh_before_remove=fresh_machine.state;
    fresh_machine.fail_apply=Resource::machine_com_x64;fresh_machine.mutate_before_failure=true;
    CHECK(uninstall(fresh_machine,machine_request).operation==E_ACCESSDENIED);
    CHECK(fresh_machine.state==fresh_before_remove);
    fresh_machine.fail_apply=Resource::count;
    CHECK(uninstall(fresh_machine,machine_request).operation==S_OK);
    CHECK(!fresh_machine.state[user_slot].present && !fresh_machine.state[machine_slot].present);
    CHECK(!fresh_machine.state[static_cast<size_t>(Resource::profile)].present);
    auto invalid_machine_request=machine_request;invalid_machine_request.include_x86=true;
    fresh_machine.events.clear();
    CHECK(install_or_repair(fresh_machine,invalid_machine_request).operation==E_INVALIDARG);
    CHECK(uninstall(fresh_machine,invalid_machine_request).operation==E_INVALIDARG);
    CHECK(fresh_machine.events.empty());
    std::cout << "installer dual-architecture lifecycle passed\n";
}
