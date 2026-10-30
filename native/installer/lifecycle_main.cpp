#include "command_line.hpp"
#include "dev_host_migration_rollback_command.hpp"
#include "dev_host_migration_inspection_command.hpp"
#include "tsf_shared_store.hpp"
#include "tray_status.hpp"
#include "windows_profile_display_repair.hpp"
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>
namespace {
struct Apartment {
    HRESULT status=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~Apartment(){if(SUCCEEDED(status))CoUninitialize();}
};
void json_string(const std::wstring& value) {
    std::cout << '"';
    for (wchar_t unit : value) {
        if (unit == L'"' || unit == L'\\') std::cout << '\\' << static_cast<char>(unit);
        else if (unit >= 32 && unit < 127) std::cout << static_cast<char>(unit);
        else std::cout << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(static_cast<uint16_t>(unit)) << std::dec;
    }
    std::cout << '"';
}
int display_command(const boshiamy::installer::InstallerCommand& command) {
    const auto report = boshiamy::installer::profile_display_repair_windows::execute(command);
    std::cout << "{\"kind\":\"profile-display-repair\",\"operation\":" << static_cast<unsigned long>(report.result.operation)
              << ",\"cleanup\":" << static_cast<unsigned long>(report.result.cleanup)
              << ",\"read_only\":" << (report.read_only ? "true" : "false")
              << ",\"ready\":" << (report.ready ? "true" : "false")
              << ",\"write_access_opened\":" << (report.write_access_opened ? "true" : "false")
              << ",\"repair_started\":" << (report.repair_started ? "true" : "false")
              << ",\"before_sha256\":\"" << report.before_sha256 << "\",\"target_sha256\":\"" << report.target_sha256
              << "\",\"branding_sha256\":\"" << report.branding_sha256 << "\",\"module\":";
    json_string(report.module);
    std::cout << ",\"ledger\":"; json_string(report.ledger);
    std::cout << "}\n";
    return boshiamy::installer::lifecycle_exit_code(report.result.operation, report.result.cleanup, S_OK);
}
int deployment_access_command(const boshiamy::installer::InstallerCommand& command) {
    using namespace boshiamy::installer::deployment_access;
    WindowsApi api;
    const Result result=check(command.expected_user_sid,api);
    std::cout<<"{\"kind\":\"deployment-access-preflight\",\"read_only\":true,\"expected_user_sid\":";
    json_string(result.expected_sid);
    std::cout<<",\"actual_user_sid\":";json_string(result.actual_sid);
    std::cout<<",\"token_status\":"<<result.token_status
             <<",\"identity_match\":"<<(result.identity_match?"true":"false")
             <<",\"requested_access\":"<<kRequestedAccess
             <<",\"category_parent\":";json_string(kCategoryParent);
    std::cout<<",\"category_checked\":"<<(result.category_checked?"true":"false")
             <<",\"category_probed_path\":";json_string(result.category_probed_path);
    std::cout<<",\"category_open_status\":";
    if(result.category_checked)std::cout<<result.category_open_status;else std::cout<<"null";
    std::cout<<",\"item_parent\":";json_string(kItemParent);
    std::cout<<",\"item_checked\":"<<(result.item_checked?"true":"false")
             <<",\"item_probed_path\":";json_string(result.item_probed_path);
    std::cout<<",\"item_open_status\":";
    if(result.item_checked)std::cout<<result.item_open_status;else std::cout<<"null";
    std::cout<<",\"access_ready\":"<<(result.passed()?"true":"false")<<"}\n";
    return result.passed()?0:1;
}
const char* resource_name(boshiamy::installer::Resource resource) {
    using boshiamy::installer::Resource;
    switch(resource) {
    case Resource::com_x64:return "com_x64";
    case Resource::com_x86:return "com_x86";
    case Resource::machine_com_x64:return "com-machine-x64";
    case Resource::profile:return "profile";
    case Resource::keyboard_category:return "keyboard_category";
    case Resource::system_tray_category:return "system_tray_category";
    case Resource::ui_element_category:return "ui_element_category";
    default:return "invalid";
    }
}
const char* trace_phase_name(boshiamy::installer::SharedStoreTracePhase phase) {
    using boshiamy::installer::SharedStoreTracePhase;
    switch (phase) {
    case SharedStoreTracePhase::Read:return "read";
    case SharedStoreTracePhase::Apply:return "apply";
    case SharedStoreTracePhase::ApplyCompensation:return "apply_compensation";
    case SharedStoreTracePhase::Restore:return "restore";
    case SharedStoreTracePhase::RestoreCompensation:return "restore_compensation";
    case SharedStoreTracePhase::Health:return "health";
    default:return "unknown";
    }
}
const char* trace_stage_name(boshiamy::installer::SharedStoreTraceStage stage) {
    using boshiamy::installer::SharedStoreTraceStage;
    switch (stage) {
    case SharedStoreTraceStage::StateRevalidation:return "state_revalidation";
    case SharedStoreTraceStage::CategoryReadback:return "category_readback";
    case SharedStoreTraceStage::MarkerRead:return "marker_read";
    case SharedStoreTraceStage::RegisterCategory:return "register_category";
    case SharedStoreTraceStage::CategoryReadbackAfterRegister:return "category_readback_after_register";
    case SharedStoreTraceStage::ProfileCapabilities:return "profile_capabilities";
    case SharedStoreTraceStage::UnregisterCategory:return "unregister_category";
    case SharedStoreTraceStage::UnregisterCategoryCompensation:return "unregister_category_compensation";
    case SharedStoreTraceStage::CategoryReadbackAfterCompensation:return "category_readback_after_compensation";
    case SharedStoreTraceStage::MarkerWrite:return "marker_write";
    case SharedStoreTraceStage::MarkerRemove:return "marker_remove";
    case SharedStoreTraceStage::FinalReadback:return "final_readback";
    case SharedStoreTraceStage::FinalVerification:return "final_verification";
    case SharedStoreTraceStage::ProfileReadback:return "profile_readback";
    case SharedStoreTraceStage::RegisterProfile:return "register_profile";
    case SharedStoreTraceStage::UnregisterProfile:return "unregister_profile";
    case SharedStoreTraceStage::ProfileIconReadback:return "profile_icon_readback";
    case SharedStoreTraceStage::ProfileUserStateRestore:return "profile_user_state_restore";
    case SharedStoreTraceStage::ProfileUserStateReadback:return "profile_user_state_readback";
    case SharedStoreTraceStage::ProfileUserStateVerification:return "profile_user_state_verification";
    case SharedStoreTraceStage::FinalUserStateVerification:return "final_user_state_verification";
    case SharedStoreTraceStage::FinalMarkerVerification:return "final_marker_verification";
    case SharedStoreTraceStage::PostApplyVerification:return "post_apply_verification";
    default:return "unknown";
    }
}
void diagnostic_trace_json(const boshiamy::installer::SharedStoreTrace& trace) {
    using namespace boshiamy::installer;
    std::cout << ",\"diagnostic_trace\":{\"events\":[";
    for (size_t i = 0; i < trace.size(); ++i) {
        const auto& event = trace.data()[i];
        if (i) std::cout << ',';
        std::cout << "{\"stage\":\"" << trace_stage_name(event.stage) << "\",\"phase\":\""
                  << trace_phase_name(event.phase) << "\",\"resource\":\"" << resource_name(event.resource)
                  << "\",\"hr\":" << static_cast<uint32_t>(event.status)
                  << ",\"present_known\":" << (event.present_known ? "true" : "false")
                  << ",\"capabilities_known\":" << (event.capabilities_known ? "true" : "false")
                  << ",\"user_state_known\":" << (event.user_state_known ? "true" : "false");
        if (event.present_known) std::cout << ",\"present\":" << (event.present ? "true" : "false");
        if (event.capabilities_known) std::cout << ",\"capabilities\":" << event.capabilities;
        if (event.required_capability) std::cout << ",\"required_capability\":" << event.required_capability;
        if (event.user_state_known) std::cout << ",\"enabled\":" << (event.enabled ? "true" : "false")
                                            << ",\"is_default\":" << (event.is_default ? "true" : "false");
        std::cout << '}';
    }
    std::cout << "],\"dropped_count\":" << trace.dropped_count() << '}';
}
void package_uninstall_json(const boshiamy::installer::package_uninstall::Report& report, bool verified_absent) {
    using namespace boshiamy::installer;
    std::wstring sid;
    deployment_access::WindowsApi identity;
    identity.process_user_sid(&sid);
    std::cout << ",\"package_uninstall\":{\"schema_version\":1,\"executed\":" << (report.executed?"true":"false")
              << ",\"registrations_absent\":" << (verified_absent?"true":"false") << ",\"install_root\":";
    json_string(report.root.wstring());
    std::cout << ",\"user_sid\":"; json_string(sid);
    std::cout << ",\"version\":"; json_string(files::detail::widen_ascii(report.installed.version));
    std::cout << ",\"manifest_sha256\":"; json_string(files::detail::widen_ascii(report.installed.manifest_sha256));
    std::cout << ",\"inventory_fingerprint\":"; json_string(files::detail::widen_ascii(report.inventory_fingerprint));
    std::cout << ",\"failure_stage\":"; json_string(files::detail::widen_ascii(report.failure_stage));
    std::cout << ",\"modern_category_cleanup_started\":" << (report.modern_category_cleanup_started?"true":"false");
    std::cout << ",\"files\":[";
    for (size_t i=0;i<report.inventory.size();++i) {
        if (i) std::cout << ',';
        const auto& file=report.inventory[i];
        std::cout << "{\"relative_path\":"; json_string(files::detail::widen_ascii(file.relative_path));
        std::cout << ",\"sha256\":"; json_string(files::detail::widen_ascii(file.sha256));
        std::cout << ",\"bytes\":" << file.bytes << '}';
    }
    std::cout << "]}";
}
HRESULT status_report(boshiamy::installer::Backend& backend,boshiamy::installer::TsfSharedApi& api,HRESULT operation,HRESULT cleanup,
                      const boshiamy::installer::SharedStoreTrace* trace,bool machine_policy=false,
                      const boshiamy::installer::package_uninstall::Report* removal=nullptr,
                      boshiamy::installer::package_categories::Store* categories=nullptr) {
    using namespace boshiamy::installer;
    const SystemTrayObservation tray=observe_system_tray(api);
    std::cout<<"{\"kind\":\"windows-installer-lifecycle\",\"operation\":"
             <<static_cast<unsigned long>(operation)<<",\"cleanup\":"<<static_cast<unsigned long>(cleanup)
             <<",\"scope\":\""<<(machine_policy?"user-and-machine-com-x64":"current-user")<<"\",\"resources\":[";
    HRESULT read_result=S_OK;
    bool all_absent=true;
    for(size_t i=0;i<static_cast<size_t>(Resource::count);++i) {
        ResourceState state{};const auto resource=static_cast<Resource>(i);
        const HRESULT observed=backend.read(resource,&state);
        all_absent=all_absent&&observed==S_OK&&state==ResourceState{};
        if(observed!=S_OK&&read_result==S_OK)read_result=observed;
        if(i)std::cout<<',';
        std::cout<<"{\"name\":\""<<resource_name(resource)<<"\",\"read_result\":"<<static_cast<unsigned long>(observed)
                 <<",\"present\":"<<(state.present?"true":"false")
                 <<",\"lifecycle_owned\":"<<(state.created_by_lifecycle?"true":"false");
        if(resource==Resource::com_x64||resource==Resource::com_x86||resource==Resource::machine_com_x64) {
            std::cout<<",\"scope\":\""<<(resource==Resource::machine_com_x64?"machine":"current-user")
                     <<"\",\"hive\":\""<<(resource==Resource::machine_com_x64?"HKLM":"HKCU")
                     <<"\",\"registry_view\":"<<(resource==Resource::com_x86?32:64)<<",\"path\":";
            json_string(state.path);std::cout<<",\"owner\":";json_string(state.owner);
        }
        std::cout<<'}';
    }
    bool enabled=false,is_default=false;
    HRESULT user_result=tray.profile_status;
    if(user_result==S_OK)user_result=tray.profile_present?api.profile_user_state(&enabled,&is_default):S_FALSE;
    // S_FALSE is expected only for skipped reads of a known absent profile.
    // An incomplete read of an existing/unknown profile must not become a
    // successful status exit or falsely report disabled/default=false.
    const bool profile_absent=tray.profile_status==S_OK&&!tray.profile_present;
    const bool user_known=profile_absent||user_result==S_OK;
    if(!profile_absent&&user_result!=S_OK&&read_result==S_OK)read_result=user_result;
    if(tray.profile_status!=S_OK&&read_result==S_OK)read_result=tray.profile_status;
    if(tray.profile_status==S_OK&&tray.profile_present&&tray.profile_caps_status!=S_OK&&read_result==S_OK)read_result=tray.profile_caps_status;
    if(tray.category_status!=S_OK&&read_result==S_OK)read_result=tray.category_status;
    std::cout<<"],\"user_enabled_result\":"<<static_cast<unsigned long>(user_result)
             <<",\"user_enabled\":"<<(user_known?(enabled?"true":"false"):"null")
             <<",\"is_default_profile\":"<<(user_known?(is_default?"true":"false"):"null")
             <<",\"profile_present_result\":"<<static_cast<unsigned long>(tray.profile_status)
             <<",\"profile_present\":"<<(tray.profile_status==S_OK?(tray.profile_present?"true":"false"):"null")
             <<",\"profile_caps_result\":"<<static_cast<unsigned long>(tray.profile_caps_status)
             <<",\"profile_caps\":"<<(tray.profile_caps_status==S_OK?std::to_string(tray.profile_caps):"null")
             <<",\"system_tray_category_result\":"<<static_cast<unsigned long>(tray.category_status)
             <<",\"system_tray_category_present\":"<<(tray.category_status==S_OK?(tray.category_present?"true":"false"):"null")
             <<",\"system_tray_supported\":"<<support_state_json(tray.support);
    if(categories) {
        package_categories::Categories present_categories;
        const HRESULT category_result=categories->read(&present_categories);
        if(category_result!=S_OK&&read_result==S_OK)read_result=category_result;
        all_absent=all_absent&&category_result==S_OK&&present_categories.empty();
        std::cout << ",\"all_categories_result\":" << static_cast<unsigned long>(category_result)
                  << ",\"all_categories_count\":";
        if(category_result==S_OK)std::cout << present_categories.size();else std::cout << "null";
        // An API enumeration that omits a malformed profile cannot authorize
        // file deletion while either exact registry leaf remains.
        if(profile_absent) {
            profile_uninstall::WindowsStore raw(boshiamy::profile_display::windows_detail::profile_key_path());
            profile_uninstall::UserState user;
            bool machine=false;
            HRESULT raw_result=raw.machine_present(&machine);
            if(raw_result==S_OK)raw_result=raw.read_user(&user);
            if(raw_result==S_OK&&(machine||user.present))raw_result=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            if(raw_result!=S_OK&&read_result==S_OK)read_result=raw_result;
            all_absent=all_absent&&raw_result==S_OK;
            std::cout << ",\"profile_registry_absence_result\":" << static_cast<unsigned long>(raw_result);
        }
    }
    if(removal&&removal->registrations_absent&&!all_absent&&read_result==S_OK)
        read_result=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    std::cout << ",\"readback\":" << static_cast<unsigned long>(read_result);
    if (trace) diagnostic_trace_json(*trace);
    if (removal) package_uninstall_json(*removal,removal->registrations_absent&&all_absent&&
        read_result==S_OK&&operation==S_OK&&cleanup==S_OK);
    std::cout<<"}\n";
    return read_result;
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace boshiamy::installer;
    if(argc==2 && std::wstring(argv[1])==independent_categories::child_argument)
        return independent_categories::child_main();
    if(argc==2 && std::wstring(argv[1])==independent_categories::caps_argument)
        return independent_categories::caps_child_main();
    if(argc==2 && std::wstring(argv[1])==independent_categories::profile_argument)
        return independent_categories::profile_child_main();
    try {
        std::vector<std::wstring> args;
        for(int i=1;i<argc;++i)args.emplace_back(argv[i]);
        const auto command=parse_installer_command(args);
        if(command.action==InstallerAction::check_deployment_access)
            return deployment_access_command(command);
        Apartment apartment;
        if(FAILED(apartment.status)){std::cerr<<"COM initialization failed\n";return 1;}
        if(command.action==InstallerAction::plan_display || command.action==InstallerAction::repair_display)
            return display_command(command);
        WindowsTsfSharedApi api;
        if(FAILED(api.ready())){std::cerr<<"TSF managers unavailable\n";return 1;}
        SharedStoreTrace trace;
        RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,command.request.owner);
        WindowsTsfSharedStore shared(api,markers,command.request.owner,command.diagnostic_trace?&trace:nullptr,
            command.action == InstallerAction::deploy_package ?
                ProfileRelocationPolicy::package_borrowed_metadata : ProfileRelocationPolicy::owned_only);
        std::unique_ptr<WindowsInstallerBackend> backend;
        const bool machine_policy=command.request.include_machine_com_x64;
        std::wstring managed_root;
        if(machine_policy&&command.action==InstallerAction::deploy_package)
            managed_root=command.deployment.install_root.lexically_normal().wstring();
        if(machine_policy&&(command.action==InstallerAction::uninstall_x64||
            command.action==InstallerAction::plan_uninstall_package||command.action==InstallerAction::uninstall_package_registration))
            managed_root=command.deployment.install_root.wstring();
        const bool read_only=command.action==InstallerAction::status||command.action==InstallerAction::inspect_dev_host_migration||
            command.action==InstallerAction::plan_uninstall_package;
        dev_host_migration_history::Lock entry_lock;
        if(!read_only) {
            const HRESULT acquired=entry_lock.acquire();
            if(acquired!=S_OK){std::cerr<<"Another lifecycle operation owns the shared registration lock\n";return 1;}
        }
        HRESULT result=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,command.request.owner,shared,&backend,
            machine_policy,managed_root,read_only?KEY_READ:KEY_READ|KEY_WRITE);
        if(FAILED(result)){std::cerr<<"Cannot open explicit COM scope (HRESULT=" << static_cast<uint32_t>(result) << ")\n";return 1;}
        // Legacy migration/recovery commands have no machine-scope ledger. They
        // must not change one half of a tracked paired registration.
        if(command.action==InstallerAction::migrate_dev_host||command.action==InstallerAction::rollback_dev_host_migration) {
            ResourceState machine;result=backend->read(Resource::machine_com_x64,&machine);
            if(result!=S_OK||machine.present) { std::cerr<<"Tracked machine COM requires the scoped lifecycle command\n";return 1; }
        }
        if(command.action==InstallerAction::migrate_dev_host ||
           command.action==InstallerAction::rollback_dev_host_migration ||
           command.action==InstallerAction::inspect_dev_host_migration) {
            boshiamy::profile_display::WindowsProfileDisplayBackend profiles;
            if(profiles.ready()!=S_OK){std::cerr<<"Profile reader unavailable\n";return 1;}
            profile_display_repair_windows::detail::Key query64,query32;
            result=query64.open_profile(KEY_WOW64_64KEY,false);
            if(result!=S_OK){std::cerr<<"Profile query unavailable\n";return 1;}
            result=query32.open_profile(KEY_WOW64_32KEY,false);
            if(result!=S_OK){std::cerr<<"Profile query unavailable\n";return 1;}
            profile_display_windows::WindowsDisplayAccess access(profiles,query64.value,query32.value,true,
                L"Global\\LiuKai-7254A21F-ProfileDisplayMetadata");
            profile_display_backend::DisplayBackend captured(*backend,access,
                profile_display_backend::Mode::existing_profile_metadata);
            if(command.action==InstallerAction::inspect_dev_host_migration) {
                const auto result=dev_host_migration_inspection_command::execute(command,captured);
                const auto& identity=result.identity;
                const char* disposition="uncertain";
                if(result.verification.disposition==dev_host_migration_recovery::Disposition::active) disposition="active";
                if(result.verification.disposition==dev_host_migration_recovery::Disposition::reversed) disposition="reversed";
                std::cout << "{\"kind\":\"x64-dev-host-migration-inspection\",\"read_only\":true,\"status\":"
                          << static_cast<unsigned long>(result.status)
                          << ",\"verification\":" << static_cast<unsigned long>(result.verification.status)
                          << ",\"disposition\":\"" << disposition << "\",\"history_checked\":"
                          << (result.history_checked?"true":"false") << ",\"history_status\":";
                if(result.history_checked) std::cout << static_cast<unsigned long>(result.history_status); else std::cout << "null";
                std::cout << ",\"schema_version\":" << identity.schema_version << ",\"owner\":";
                json_string(files::detail::widen_ascii(identity.owner));
                std::cout << ",\"install_root\":";json_string(identity.install_root);
                std::cout << ",\"storage_root\":";json_string(identity.storage_root);
                std::cout << ",\"ledger_dir\":";json_string(identity.storage_root);
                std::cout << ",\"token\":";json_string(files::detail::widen_ascii(identity.token));
                std::cout << ",\"package_version\":";json_string(files::detail::widen_ascii(identity.package_version));
                std::cout << ",\"package_manifest_sha256\":";json_string(files::detail::widen_ascii(identity.package_manifest_sha256));
                std::cout << ",\"forward_journal_sha256\":";json_string(files::detail::widen_ascii(command.expected_forward_journal_sha256));
                std::cout << ",\"original_snapshot_sha256\":";json_string(files::detail::widen_ascii(command.expected_original_snapshot_sha256));
                std::cout << ",\"verified_resource_count\":" << (result.status==S_OK?6:0)
                          << ",\"terminal_compensated_ledgers\":[";
                for(size_t i=0;i<result.terminal_ledgers.size();++i) {
                    const auto& record=result.terminal_ledgers[i];const auto& tid=record.identity;
                    if(i)std::cout << ',';
                    std::cout << "{\"schema_version\":" << tid.schema_version << ",\"owner\":";
                    json_string(files::detail::widen_ascii(tid.owner));
                    std::cout << ",\"install_root\":";json_string(tid.install_root);
                    std::cout << ",\"ledger_dir\":";json_string(tid.storage_root);
                    std::cout << ",\"token\":";json_string(files::detail::widen_ascii(tid.token));
                    std::cout << ",\"package_version\":";json_string(files::detail::widen_ascii(tid.package_version));
                    std::cout << ",\"package_manifest_sha256\":";json_string(files::detail::widen_ascii(tid.package_manifest_sha256));
                    std::cout << ",\"forward_journal_sha256\":";json_string(files::detail::widen_ascii(record.forward_journal_sha256));
                    std::cout << ",\"original_snapshot_sha256\":";json_string(files::detail::widen_ascii(record.original_snapshot_sha256));
                    std::cout << ",\"terminal_marker_sha256\":";json_string(files::detail::widen_ascii(record.terminal_marker_sha256));
                    std::cout << ",\"prestate_exact\":" << (record.prestate_exact?"true":"false")
                              << ",\"file_rollback_exact\":" << (record.file_rollback_exact?"true":"false") << '}';
                }
                std::cout << "]}\n";
                return result.status==S_OK ? 0 : 1;
            }
            if(command.action==InstallerAction::rollback_dev_host_migration) {
                const auto rolled=dev_host_migration_rollback_command::execute(command,captured);
                ResourceStates state{};
                const HRESULT observed=detail::read_all(captured,state);
                const char* disposition="uncertain";
                if(rolled.verification.disposition==dev_host_migration_recovery::Disposition::active)
                    disposition="active";
                if(rolled.verification.disposition==dev_host_migration_recovery::Disposition::reversed)
                    disposition="reversed";
                std::cout<<"{\"kind\":\"x64-dev-host-migration-rollback\",\"operation\":"
                         <<static_cast<unsigned long>(rolled.transaction.operation)
                         <<",\"cleanup\":"<<static_cast<unsigned long>(rolled.transaction.cleanup)
                         <<",\"readback\":"<<static_cast<unsigned long>(observed)
                         <<",\"verification\":"<<static_cast<unsigned long>(rolled.verification.status)
                         <<",\"disposition\":\""<<disposition<<"\",\"current_x64_path\":";
                json_string(state[0].path);
                std::cout<<"}\n";
                return dev_host_migration_rollback_command::exit_code(rolled,observed);
            }
            const auto transaction=execute_installer_command(captured,command);
            ResourceStates state{};
            const HRESULT observed=detail::read_all(captured,state);
            std::cout<<"{\"kind\":\"x64-dev-host-migration\",\"operation\":"<<static_cast<unsigned long>(transaction.operation)
                     <<",\"cleanup\":"<<static_cast<unsigned long>(transaction.cleanup)
                     <<",\"readback\":"<<static_cast<unsigned long>(observed)<<",\"current_x64_path\":";
            json_string(state[0].path);
            if(command.diagnostic_trace) diagnostic_trace_json(trace);
            std::cout<<"}\n";
            return lifecycle_exit_code(transaction.operation,transaction.cleanup,observed);
        }
        const bool package_removal=command.action==InstallerAction::plan_uninstall_package||
            command.action==InstallerAction::uninstall_package_registration;
        package_uninstall::Report removal;
        package_categories::WindowsStore categories;
        auto transaction=execute_installer_command(*backend,command,package_removal?&removal:nullptr,&categories);
        if(command.action==InstallerAction::uninstall_package_registration) {
            // Reopen both TSF managers and registry adapters. The package wrapper
            // additionally checks --status in a separate process before deletion.
            WindowsTsfSharedApi verified_api;
            WindowsTsfSharedStore verified_shared(verified_api,markers,command.request.owner);
            std::unique_ptr<WindowsInstallerBackend> verified_backend;
            HRESULT verified=verified_api.ready();
            if(verified==S_OK) verified=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,
                command.request.owner,verified_shared,&verified_backend,true,managed_root,KEY_READ);
            if(verified==S_OK) {
                package_categories::WindowsStore verified_categories;
                const HRESULT observed=status_report(*verified_backend,verified_api,transaction.operation,transaction.cleanup,
                    command.diagnostic_trace?&trace:nullptr,true,&removal,&verified_categories);
                return lifecycle_exit_code(transaction.operation,transaction.cleanup,observed);
            }
            if(transaction.operation==S_OK)transaction.operation=verified;
            removal.failure_stage="independent-registration-readback";
        }
        const HRESULT observed=status_report(*backend,api,transaction.operation,transaction.cleanup,
            command.diagnostic_trace?&trace:nullptr,machine_policy,package_removal?&removal:nullptr,&categories);
        return lifecycle_exit_code(transaction.operation,transaction.cleanup,observed);
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<"\nUsage: LiuKaiInstaller --status [--machine-com-x64] OR "
            "--install|--repair|--uninstall <x64 DLL> <x86 DLL> <stable icon DLL> <expected old x64 DLL|-> <expected old x86 DLL|-> OR "
            "--uninstall-x64 <expected old x64 DLL> [--machine-com-x64] OR "
            "--plan-uninstall-package <managed root> OR --uninstall-package-registration <managed root> [--diagnostic-trace] OR "
            "--migrate-dev-host <x64 package root> <new managed root> <expected old x64 DLL> <expected old DLL SHA-256> OR "
            "--rollback-dev-host-migration <managed root> <ledger dir> <expected token> <forward journal SHA-256> <original snapshot SHA-256> OR "
            "--inspect-dev-host-migration <managed root> <ledger dir> <expected token> <forward journal SHA-256> <original snapshot SHA-256> OR "
            "--check-deployment-access <expected-user-SID> OR "
            "--plan-display <installed root> OR --repair-display <installed root> <expected full PDS1 SHA-256> <expected branding SHA-256> OR "
            "--deploy-package <package root> <install root> <expected old version|-> <expected old CLI SHA-256|-> <expected old branding SHA-256|-> [--machine-com-x64]\n";
        return 2;
    }
}
