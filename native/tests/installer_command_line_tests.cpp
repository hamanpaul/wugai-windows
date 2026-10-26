#include "../installer/command_line.hpp"
#include <iostream>
#define CHECK(x) do{if(!(x)){std::cerr<<#x<<" line "<<__LINE__<<'\n';return 1;}}while(0)
using namespace boshiamy::installer;
struct CommandBackend final:Backend {
    std::array<ResourceState,static_cast<size_t>(Resource::count)> states{};
    unsigned writes=0,x64_reads=0;
    unsigned health_checks=0;
    HRESULT health_result=S_OK;
    HRESULT fail_apply_result=E_ACCESSDENIED;
    Resource fail_apply_resource=Resource::count;
    Resource s_false_apply_no_write=Resource::count;
    Resource s_false_restore_no_write=Resource::count;
    HRESULT read_result=S_OK;
    unsigned reads=0;
    unsigned s_false_after_read=0;
    std::array<unsigned,static_cast<size_t>(Resource::count)> reads_by_resource{};
    Resource s_false_resource=Resource::count;
    unsigned s_false_resource_read_number=0;
    bool leave_read_output_untouched=false;
    bool fail_reads=false,appear_on_second_x64_read=false;
    HRESULT read(Resource resource,ResourceState* out)override {
        if(fail_reads)return E_ACCESSDENIED;
        ++reads;
        const unsigned resource_read=++reads_by_resource[static_cast<size_t>(resource)];
        if(s_false_after_read&&reads>s_false_after_read)return S_FALSE;
        if(resource==s_false_resource&&resource_read==s_false_resource_read_number)return S_FALSE;
        if(resource==Resource::com_x64&&++x64_reads==2&&appear_on_second_x64_read) {
            states[0].present=true;states[0].owner=L"LiuKai-TSF-v1";
            states[0].path=L"C:\\unexpected\\BoshiamyTsf.dll";states[0].threading_model=L"Apartment";
        }
        if(read_result!=S_OK&&leave_read_output_untouched)return read_result;
        *out=states[static_cast<size_t>(resource)];return read_result;
    }
    HRESULT apply(Resource resource,const ResourceState& state)override{
        ++writes;
        if(resource==s_false_apply_no_write)return S_FALSE;
        if(resource==fail_apply_resource)return fail_apply_result;
        states[static_cast<size_t>(resource)]=state;return S_OK;
    }
    HRESULT restore(Resource resource,const ResourceState& state)override{
        if(resource==s_false_restore_no_write)return S_FALSE;
        states[static_cast<size_t>(resource)]=state;return S_OK;
    }
    HRESULT verify_install_health()override{++health_checks;return health_result;}
};
// Parser-only RED coverage: use existing command fields so this compiles before
// the machine-COM policy is added. No package reads or command execution here.
static int machine_com_x64_argument_tests() {
    struct AcceptedCase {
        const char* name;
        std::vector<std::wstring> arguments;
        InstallerAction action;
    };
    const std::vector<std::wstring> deploy={L"--deploy-package",L"C:\\LiuKaiPackage",L"C:\\LiuKai",L"-",L"-",L"-"};
    const std::vector<std::wstring> status={L"--status"};
    const std::vector<std::wstring> uninstall={L"--uninstall-x64",L"C:\\LiuKai\\versions\\v59\\x64\\BoshiamyTsf.dll"};
    auto flagged_deploy=deploy;flagged_deploy.push_back(L"--machine-com-x64");
    auto flagged_upgrade=deploy;
    flagged_upgrade[3]=L"v59";
    flagged_upgrade[4]=std::wstring(64,L'a');
    flagged_upgrade[5]=std::wstring(64,L'b');
    flagged_upgrade.push_back(L"--machine-com-x64");
    auto flagged_status=status;flagged_status.push_back(L"--machine-com-x64");
    auto flagged_uninstall=uninstall;flagged_uninstall.push_back(L"--machine-com-x64");
    const AcceptedCase accepted_cases[]={
        {"fresh deploy-package",flagged_deploy,InstallerAction::deploy_package},
        {"upgrade deploy-package",flagged_upgrade,InstallerAction::deploy_package},
        {"read-only status",flagged_status,InstallerAction::status},
        {"x64-only uninstall",flagged_uninstall,InstallerAction::uninstall_x64}
    };
    for(const auto& test:accepted_cases) {
        InstallerCommand command{};
        bool accepted=false;
        try {command=parse_installer_command(test.arguments);accepted=true;}
        catch(const std::exception& error) {
            std::cerr<<"valid "<<test.name<<" rejected trailing --machine-com-x64: "<<error.what()<<'\n';
        }
        CHECK(accepted);
        CHECK(command.action==test.action);
        if(test.action==InstallerAction::deploy_package) {
            CHECK(command.deployment.package_root==std::filesystem::path(deploy[1]));
            CHECK(command.deployment.install_root==std::filesystem::path(deploy[2]));
            CHECK(command.deployment.expected_old_version==(test.arguments[3]==L"-"?"":"v59"));
        }
        if(test.action==InstallerAction::uninstall_x64) {
            CHECK(!command.request.include_x86);
            CHECK(command.request.expected_x64_path==uninstall[1]);
            CHECK(command.request.expected_x86_path.empty());
        }
    }
    // Existing unflagged forms still parse with their original action/path data.
    CHECK(parse_installer_command(deploy).action==InstallerAction::deploy_package);
    CHECK(parse_installer_command(status).action==InstallerAction::status);
    CHECK(parse_installer_command(uninstall).request.expected_x64_path==uninstall[1]);
    std::vector<std::vector<std::wstring>> rejected_cases;
    for(const auto& base:{deploy,status,uninstall}) {
        auto duplicate=base;
        duplicate.push_back(L"--machine-com-x64");
        duplicate.push_back(L"--machine-com-x64");
        rejected_cases.push_back(duplicate);
        auto misplaced=base;
        misplaced.insert(misplaced.begin()+(misplaced.size()>1?1:0),L"--machine-com-x64");
        rejected_cases.push_back(misplaced);
    }
    rejected_cases.push_back({L"--plan-display",L"C:\\LiuKai",L"--machine-com-x64"});
    rejected_cases.push_back({L"--check-deployment-access",L"S-1-5-21-1-2-3-1001",L"--machine-com-x64"});
    for(const auto* action:{L"--install",L"--repair",L"--uninstall"})
        rejected_cases.push_back({action,L"C:\\LiuKai\\x64\\service.dll",L"C:\\LiuKai\\x86\\service.dll",
            L"C:\\LiuKai\\shared\\icons.dll",L"C:\\LiuKai\\x64\\service.dll",L"C:\\LiuKai\\x86\\service.dll",L"--machine-com-x64"});
    for(const auto& arguments:rejected_cases) {
        bool refused=false;
        try {(void)parse_installer_command(arguments);}
        catch(const std::invalid_argument&) {refused=true;}
        if(!refused)std::cerr<<"misplaced, duplicate, or inapplicable --machine-com-x64 accepted\n";
        CHECK(refused);
    }
    return 0;
}

int main() {
    CHECK(machine_com_x64_argument_tests()==0);
    const std::vector<std::wstring> install_args={L"--install",L"C:\\LiuKai\\x64\\service.dll",L"C:\\LiuKai\\x86\\service.dll",L"C:\\LiuKai\\shared\\icons.dll",L"-",L"-"};
    const auto install=parse_installer_command(install_args);
    CHECK(install.request.owner==L"LiuKai-TSF-v1");
    CHECK(install.request.x64_binary_path!=install.request.x86_binary_path);
    CHECK(install.request.expected_x64_path.empty());
    auto traced_install_args=install_args;traced_install_args.push_back(L"--diagnostic-trace");
    const auto traced_install=parse_installer_command(traced_install_args);
    CHECK(traced_install.action==InstallerAction::install&&traced_install.diagnostic_trace);
    CHECK(!install.diagnostic_trace);
    auto misplaced_trace_args=install_args;misplaced_trace_args.insert(misplaced_trace_args.begin()+2,L"--diagnostic-trace");
    bool misplaced_trace_refused=false;try{parse_installer_command(misplaced_trace_args);}catch(const std::invalid_argument&){misplaced_trace_refused=true;}
    CHECK(misplaced_trace_refused);
    bool status_trace_refused=false;try{parse_installer_command({L"--status",L"--diagnostic-trace"});}catch(const std::invalid_argument&){status_trace_refused=true;}
    CHECK(status_trace_refused);
    bool read_only_trace_refused=false;try{parse_installer_command({L"--plan-display",L"C:\\LiuKai",L"--diagnostic-trace"});}catch(const std::invalid_argument&){read_only_trace_refused=true;}
    CHECK(read_only_trace_refused);
    for(auto bad:std::vector<std::vector<std::wstring>>{{},{L"--status",L"extra"},
            {L"--unknown",L"C:\\a",L"C:\\b",L"C:\\c",L"-",L"-"},
            {L"--install",L"relative.dll",L"C:\\b",L"C:\\c",L"-",L"-"},
            {L"--uninstall",L"C:\\a",L"C:\\b",L"C:\\c",L"-",L"-"}}) {
        bool refused=false;try{parse_installer_command(bad);}catch(const std::invalid_argument&){refused=true;}CHECK(refused);
    }

    // S_FALSE with no output must not be interpreted as an absent COM view.
    // These checks intentionally fail against FAILED(hr)-only command guards.
    CommandBackend partial_read;
    partial_read.read_result=S_FALSE;
    partial_read.leave_read_output_untouched=true;
    CommandBackend partial_install=partial_read;
    partial_install.states[static_cast<size_t>(Resource::com_x64)]={true,false,L"Foreign",L"C:\\foreign\\service.dll",L"Apartment",{}};
    const auto partial_install_result=execute_installer_command(partial_install,install);
    if(partial_install_result.operation!=S_FALSE) {
        std::cerr<<"S_FALSE no-output preflight accepted as status="<<std::hex<<partial_install_result.operation
                 <<" writes="<<std::dec<<partial_install.writes<<" com64="
                 <<std::string(partial_install.states[0].path.begin(),partial_install.states[0].path.end())<<'\n';
        return 1;
    }
    CHECK(partial_install.writes==0);
    CHECK(validate_package_absence(partial_read)==S_FALSE);
    CHECK(partial_read.writes==0);
    CHECK(execute_installer_command(partial_read,parse_installer_command({L"--status"})).operation==S_FALSE);
    CHECK(partial_read.writes==0);
    CHECK(execute_installer_command(partial_read,install).operation==S_FALSE);
    CHECK(partial_read.writes==0);
    auto partial_repair_args=install_args;partial_repair_args[0]=L"--repair";
    CHECK(execute_installer_command(partial_read,parse_installer_command(partial_repair_args)).operation==S_FALSE);
    CHECK(partial_read.writes==0);
    const auto partial_uninstall=parse_installer_command({L"--uninstall",install_args[1],install_args[2],install_args[3],install_args[1],install_args[2]});
    CHECK(execute_installer_command(partial_read,partial_uninstall).operation==S_FALSE);
    CHECK(partial_read.writes==0);
    const auto partial_uninstall_x64=parse_installer_command({L"--uninstall-x64",install_args[1]});
    CHECK(execute_installer_command(partial_read,partial_uninstall_x64).operation==S_FALSE);
    CHECK(partial_read.writes==0);

    // In x64-only uninstall, a failed COM32 guard read must retain its exact
    // HRESULT and roll the already removed COM64 view back without touching
    // shared state or COM32.
    CommandBackend guard_read_failure;
    guard_read_failure.s_false_resource=Resource::com_x86;
    guard_read_failure.s_false_resource_read_number=2; // initial snapshot, then pre-shared-mutation guard
    guard_read_failure.states[static_cast<size_t>(Resource::com_x64)]={true,true,L"LiuKai-TSF-v1",install_args[1],L"Apartment",{}};
    guard_read_failure.states[static_cast<size_t>(Resource::profile)]={true,true,L"LiuKai-TSF-v1",install_args[3],L"",{}};
    guard_read_failure.states[static_cast<size_t>(Resource::keyboard_category)]={true,true,L"LiuKai-TSF-v1",L"",L"",{}};
    guard_read_failure.states[static_cast<size_t>(Resource::system_tray_category)]={true,true,L"LiuKai-TSF-v1",L"",L"",{}};
    const auto guard_read_result=execute_installer_command(guard_read_failure,partial_uninstall_x64);
    CHECK(guard_read_result.operation==S_FALSE&&guard_read_result.cleanup==S_OK);
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::com_x64)].present);
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::com_x86)]==ResourceState{});
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::profile)].present);
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::keyboard_category)].present);
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::system_tray_category)].present);

    CommandBackend second_snapshot;
    second_snapshot.s_false_after_read=2; // preflight succeeds; transaction snapshot is partial.
    second_snapshot.leave_read_output_untouched=true;
    const auto second_snapshot_result=execute_installer_command(second_snapshot,install);
    if(second_snapshot_result.operation!=S_FALSE||second_snapshot.writes!=0)
        std::cerr<<"second snapshot status="<<std::hex<<second_snapshot_result.operation<<" writes="<<std::dec<<second_snapshot.writes<<" reads="<<second_snapshot.reads<<'\n';
    CHECK(second_snapshot_result.operation==S_FALSE&&second_snapshot.writes==0);

    CommandBackend positive_health_failure;
    positive_health_failure.health_result=S_FALSE;
    const auto positive_health_result=execute_installer_command(positive_health_failure,install);
    CHECK(positive_health_result.operation==S_FALSE&&positive_health_result.cleanup==S_OK);
    for(const auto& state:positive_health_failure.states)CHECK(state==ResourceState{});

    CHECK(lifecycle_exit_code(S_OK,S_OK,S_OK)==0);
    CHECK(lifecycle_exit_code(S_FALSE,S_OK,S_OK)!=0);
    CHECK(lifecycle_exit_code(S_OK,S_FALSE,S_OK)!=0);
    CHECK(lifecycle_exit_code(S_OK,S_OK,S_FALSE)!=0);

    // Mutation adapters have no documented S_FALSE-success contract; a
    // positive no-write result must stop the transaction and retain its HRESULT.
    CommandBackend apply_s_false;
    apply_s_false.s_false_apply_no_write=Resource::com_x64;
    const auto apply_s_false_result=execute_installer_command(apply_s_false,install);
    if(apply_s_false_result.operation!=S_FALSE||apply_s_false_result.cleanup!=S_OK)
        std::cerr<<"apply S_FALSE op="<<std::hex<<apply_s_false_result.operation<<" cleanup="<<apply_s_false_result.cleanup
                 <<" apply_calls="<<std::dec<<apply_s_false.writes<<" com64_present="
                 <<apply_s_false.states[static_cast<size_t>(Resource::com_x64)].present<<"\n";
    CHECK(apply_s_false_result.operation==S_FALSE&&apply_s_false_result.cleanup==S_OK);
    CHECK(apply_s_false.writes==1&&apply_s_false.health_checks==0);
    for(const auto& state:apply_s_false.states)CHECK(state==ResourceState{});

    // A positive restore result is not cleanup success, even when the fake
    // deliberately leaves the resource at the just-installed value.
    CommandBackend restore_s_false;
    restore_s_false.fail_apply_resource=Resource::com_x86;
    restore_s_false.s_false_restore_no_write=Resource::com_x64;
    const auto restore_s_false_result=execute_installer_command(restore_s_false,install);
    CHECK(restore_s_false_result.operation==E_ACCESSDENIED&&restore_s_false_result.cleanup==S_FALSE);
    CHECK(restore_s_false.states[static_cast<size_t>(Resource::com_x64)].present);

    CommandBackend fresh;
    CHECK(execute_installer_command(fresh,install).operation==S_OK);
    CHECK(fresh.writes==6&&fresh.health_checks==1);
    CHECK(execute_installer_command(fresh,install).operation==S_OK);CHECK(fresh.writes==6&&fresh.health_checks==2);
    fresh.states[static_cast<size_t>(Resource::system_tray_category)]={};
    auto repair_args=install_args;repair_args[0]=L"--repair";
    CHECK(execute_installer_command(fresh,parse_installer_command(repair_args)).operation==S_OK);
    CHECK(fresh.writes==7&&fresh.health_checks==3);

    // CLI install must propagate a failed platform health check and restore
    // every resource it just applied.
    CommandBackend unhealthy_install;
    unhealthy_install.health_result=E_ACCESSDENIED;
    const auto unhealthy_install_result=execute_installer_command(unhealthy_install,install);
    CHECK(unhealthy_install_result.operation==E_ACCESSDENIED&&SUCCEEDED(unhealthy_install_result.cleanup));
    CHECK(unhealthy_install.health_checks==1);
    for(const auto& state:unhealthy_install.states)CHECK(state==ResourceState{});

    // A healthy install followed by a changed-path repair whose postcheck
    // fails must restore the original COM paths and shared resources.
    CommandBackend repair_backend;
    CHECK(execute_installer_command(repair_backend,install).operation==S_OK);
    const auto installed_state=repair_backend.states;
    repair_backend.health_result=E_ACCESSDENIED;
    const auto no_op_install_result=execute_installer_command(repair_backend,install);
    CHECK(no_op_install_result.operation==E_ACCESSDENIED&&SUCCEEDED(no_op_install_result.cleanup));
    CHECK(repair_backend.health_checks==2&&repair_backend.writes==6&&repair_backend.states==installed_state);
    auto no_op_repair_args=repair_args;
    no_op_repair_args[4]=install.request.x64_binary_path;
    no_op_repair_args[5]=install.request.x86_binary_path;
    const auto no_op_repair_result=execute_installer_command(repair_backend,parse_installer_command(no_op_repair_args));
    CHECK(no_op_repair_result.operation==E_ACCESSDENIED&&SUCCEEDED(no_op_repair_result.cleanup));
    CHECK(repair_backend.health_checks==3&&repair_backend.writes==6&&repair_backend.states==installed_state);
    auto changed_repair_args=repair_args;
    changed_repair_args[1]=L"C:\\LiuKai\\x64\\v2\\service.dll";
    changed_repair_args[2]=L"C:\\LiuKai\\x86\\v2\\service.dll";
    changed_repair_args[4]=install.request.x64_binary_path;
    changed_repair_args[5]=install.request.x86_binary_path;
    const auto unhealthy_repair_result=execute_installer_command(repair_backend,parse_installer_command(changed_repair_args));
    CHECK(unhealthy_repair_result.operation==E_ACCESSDENIED&&SUCCEEDED(unhealthy_repair_result.cleanup));
    CHECK(repair_backend.health_checks==4);
    CHECK(repair_backend.states==installed_state);
    CommandBackend conflict;
    conflict.states[0]={true,false,L"LiuKai-TSF-v1",L"C:\\unexpected\\service.dll",L"Apartment",{}};
    CHECK(FAILED(execute_installer_command(conflict,install).operation));CHECK(conflict.writes==0);
    CommandBackend race;race.appear_on_second_x64_read=true;
    CHECK(FAILED(execute_installer_command(race,install).operation));CHECK(race.writes==0);
    CHECK(race.states[0].path==L"C:\\unexpected\\BoshiamyTsf.dll");
    CommandBackend inspect;
    const auto status=parse_installer_command({L"--status"});
    CHECK(execute_installer_command(inspect,status).operation==S_OK);CHECK(inspect.writes==0);
    inspect.fail_reads=true;
    CHECK(execute_installer_command(inspect,status).operation==E_ACCESSDENIED);CHECK(inspect.writes==0);
    std::cout<<"Installer command validation, read-only status, and expected-absence race guard passed\n";
}
