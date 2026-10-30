#ifndef LIUKAI_INSTALLER_COMMAND_LINE_HPP
#define LIUKAI_INSTALLER_COMMAND_LINE_HPP
#include "deployment_coordinator.hpp"
#include "durable_cli_session.hpp"
#include "dev_host_package_migration.hpp"
#include "deployment_access_preflight.hpp"
#include "package_uninstall.hpp"
#include <algorithm>
#include <stdexcept>

namespace boshiamy::installer {

enum class InstallerAction { status, install, repair, uninstall, deploy_package, uninstall_x64, plan_display, repair_display, migrate_dev_host, rollback_dev_host_migration, inspect_dev_host_migration, check_deployment_access, plan_uninstall_package, uninstall_package_registration };
struct InstallerCommand {
    InstallerAction action;
    InstallRequest request;
    files::DeploymentRequest deployment;
    std::string expected_display_sha256;
    std::string expected_branding_sha256;
    std::string expected_old_dll_sha256;
    std::filesystem::path rollback_ledger_dir;
    std::string rollback_token;
    std::string expected_forward_journal_sha256;
    std::string expected_original_snapshot_sha256;
    std::wstring expected_user_sid;
    bool diagnostic_trace = false;
};

inline bool all_ascii_token(const std::wstring& value, std::string* out) {
    if (!out || value.empty()) return false;
    out->clear();
    for (wchar_t c : value) {
        if (c < 0x20 || c > 0x7e) return false;
        out->push_back(static_cast<char>(c));
    }
    return true;
}

inline bool canonical_rollback_path(const std::wstring& value) {
    if (!detail::absolute_path(value) || value.find(L'\0') != std::wstring::npos) return false;
    try {
        const std::filesystem::path path(value);
        if (path.filename().empty() || path != path.lexically_normal()) return false;
        for (const auto& component : path)
            if (component == L"." || component == L"..") return false;
        return true;
    } catch (...) { return false; }
}

inline std::filesystem::path machine_com_uninstall_root(const std::wstring& value) {
    if(!canonical_rollback_path(value))throw std::invalid_argument("Machine uninstall requires a canonical installed DLL path");
    const std::filesystem::path dll(value);
    const auto version=dll.parent_path().parent_path();
    if(_wcsicmp(dll.filename().c_str(),L"BoshiamyTsf.dll")!=0||
       _wcsicmp(dll.parent_path().filename().c_str(),L"x64")!=0||
       _wcsicmp(version.parent_path().filename().c_str(),L"versions")!=0||version.filename().empty())
        throw std::invalid_argument("Machine uninstall requires a managed version x64 DLL path");
    return version.parent_path().parent_path();
}

inline int lifecycle_exit_code(HRESULT operation, HRESULT cleanup, HRESULT observed) {
    return operation == S_OK && cleanup == S_OK && observed == S_OK ? 0 : 1;
}

inline InstallerCommand parse_installer_command(const std::vector<std::wstring>& input_args) {
    std::vector<std::wstring> args = input_args;
    InstallerCommand command{};
    command.request.owner=L"LiuKai-TSF-v1";
    command.deployment.owner="LiuKai-TSF-v1";
    const auto machine_flag=std::find(args.begin(),args.end(),L"--machine-com-x64");
    if(machine_flag!=args.end()) {
        const bool supported=!args.empty()&&(args[0]==L"--deploy-package"||args[0]==L"--status"||args[0]==L"--uninstall-x64");
        if(!supported||machine_flag!=args.end()-1)
            throw std::invalid_argument("--machine-com-x64 must be the final argument on deploy-package/status/uninstall-x64");
        args.pop_back();
        command.request.include_machine_com_x64=true;
        command.request.include_x86=false;
        command.deployment.include_machine_com_x64=true;
    }
    const auto trace_flag = std::find(args.begin(), args.end(), L"--diagnostic-trace");
    if (trace_flag != args.end()) {
        if (trace_flag + 1 != args.end() || trace_flag != args.end() - 1)
            throw std::invalid_argument("--diagnostic-trace must be the final argument");
        const bool supported_action = !args.empty() &&
            (args[0] == L"--install" || args[0] == L"--repair" || args[0] == L"--uninstall" ||
             args[0] == L"--uninstall-x64" || args[0] == L"--deploy-package" ||
             args[0] == L"--migrate-dev-host" || args[0] == L"--uninstall-package-registration");
        if (!supported_action)
            throw std::invalid_argument("--diagnostic-trace is supported only for TSF registration actions");
        args.pop_back();
        command.diagnostic_trace = true;
    }
    if (args.size()==2 && (args[0]==L"--plan-uninstall-package" || args[0]==L"--uninstall-package-registration")) {
        if (!canonical_rollback_path(args[1])) throw std::invalid_argument("Package uninstall requires a canonical absolute root");
        command.action=args[0]==L"--plan-uninstall-package" ? InstallerAction::plan_uninstall_package : InstallerAction::uninstall_package_registration;
        command.deployment.install_root=args[1];
        command.deployment.include_machine_com_x64=true;
        command.request.include_machine_com_x64=true;
        command.request.include_x86=false;
        return command;
    }
    if(args.size()==1&&args[0]==L"--status") {
        command.action=InstallerAction::status;
        return command;
    }
    if(args.size()==2&&args[0]==L"--check-deployment-access") {
        if(!deployment_access::canonical_sid(args[1]))
            throw std::invalid_argument("Expected user SID must be a canonical SID");
        command.action=InstallerAction::check_deployment_access;
        command.expected_user_sid=args[1];
        return command;
    }
    if (args.size()==5 && args[0]==L"--migrate-dev-host") {
        for (size_t i=1;i!=4;++i)
            if (!profile_display_plan::detail::valid_stable_module(args[i]))
                throw std::invalid_argument("Migration package/root/old DLL paths must be absolute");
        if (!all_ascii_token(args[4],&command.expected_old_dll_sha256) ||
            !files::detail::valid_hash(command.expected_old_dll_sha256))
            throw std::invalid_argument("Migration requires expected old DLL SHA-256");
        command.action=InstallerAction::migrate_dev_host;
        command.deployment.package_root=args[1]; command.deployment.install_root=args[2];
        command.request.expected_x64_path=args[3]; command.request.include_x86=false;
        return command;
    }
    if (args.size()==6 && (args[0]==L"--rollback-dev-host-migration" ||
                          args[0]==L"--inspect-dev-host-migration")) {
        for (size_t i=1;i!=3;++i)
            if (!canonical_rollback_path(args[i]))
                throw std::invalid_argument("Rollback managed root and ledger must be absolute paths");
        if (!all_ascii_token(args[3], &command.rollback_token) || command.rollback_token.size()!=32 ||
            !std::all_of(command.rollback_token.begin(), command.rollback_token.end(),
                         [](char c) { return (c>='0'&&c<='9') || (c>='a'&&c<='f'); }) ||
            !all_ascii_token(args[4], &command.expected_forward_journal_sha256) ||
            !files::detail::valid_hash(command.expected_forward_journal_sha256) ||
            !all_ascii_token(args[5], &command.expected_original_snapshot_sha256) ||
            !files::detail::valid_hash(command.expected_original_snapshot_sha256))
            throw std::invalid_argument("Rollback token or pinned SHA-256 is malformed");
        command.action=args[0]==L"--inspect-dev-host-migration" ?
            InstallerAction::inspect_dev_host_migration : InstallerAction::rollback_dev_host_migration;
        command.deployment.install_root=args[1];
        command.rollback_ledger_dir=args[2];
        if (command.rollback_ledger_dir.filename().wstring()!=args[3])
            throw std::invalid_argument("Rollback ledger basename must match token");
        return command;
    }
    if ((args.size()==2 && args[0]==L"--plan-display") ||
        (args.size()==4 && args[0]==L"--repair-display")) {
        command.action=args[0]==L"--plan-display"?InstallerAction::plan_display:InstallerAction::repair_display;
        if (!detail::absolute_path(args[1]) || args[1].find(L'\0')!=std::wstring::npos)
            throw std::invalid_argument("Installed package root must be absolute");
        command.deployment.install_root=args[1];
        if (command.action==InstallerAction::repair_display &&
            (!all_ascii_token(args[2],&command.expected_display_sha256) ||
             !files::detail::valid_hash(command.expected_display_sha256) ||
             !all_ascii_token(args[3],&command.expected_branding_sha256) ||
             !files::detail::valid_hash(command.expected_branding_sha256)))
            throw std::invalid_argument("Display repair requires full snapshot and branding SHA-256 tokens");
        return command;
    }
    if(args.size()==2&&args[0]==L"--uninstall-x64") {
        if(!detail::absolute_path(args[1]))throw std::invalid_argument("Expected x64 DLL path must be absolute");
        command.action=InstallerAction::uninstall_x64;
        command.request.include_x86=false;
        command.request.x64_binary_path=args[1];
        command.request.expected_x64_path=args[1];
        if(command.request.include_machine_com_x64)command.deployment.install_root=machine_com_uninstall_root(args[1]);
        return command;
    }
    if (args.size()==6 && args[0]==L"--deploy-package") {
        command.action=InstallerAction::deploy_package;
        for (const auto& path : {args[1], args[2]})
            if (!detail::absolute_path(path)) throw std::invalid_argument("Package and install roots must be absolute");
        command.deployment.package_root=std::filesystem::path(args[1]);
        command.deployment.install_root=std::filesystem::path(args[2]);
        command.deployment.expected_old_version.clear();
        command.deployment.expected_old_cli_sha256.clear();
        command.deployment.expected_old_branding_sha256.clear();
        if (args[3]==L"-" && args[4]==L"-" && args[5]==L"-") return command;
        if (args[3]==L"-" || args[4]==L"-" || args[5]==L"-")
            throw std::invalid_argument("Expected-old version and hashes must be all '-' or all explicit");
        std::string version, cli_hash, branding_hash;
        if (!all_ascii_token(args[3], &version) || !files::detail::safe_label(version) ||
            !all_ascii_token(args[4], &cli_hash) || !files::detail::valid_hash(cli_hash) ||
            !all_ascii_token(args[5], &branding_hash) || !files::detail::valid_hash(branding_hash))
            throw std::invalid_argument("Expected-old version/hash token is malformed");
        command.deployment.expected_old_version=std::move(version);
        command.deployment.expected_old_cli_sha256=std::move(cli_hash);
        command.deployment.expected_old_branding_sha256=std::move(branding_hash);
        return command;
    }
    if(args.size()!=6)throw std::invalid_argument("Expected action and five absolute path arguments");
    if(args[0]==L"--install")command.action=InstallerAction::install;
    else if(args[0]==L"--repair")command.action=InstallerAction::repair;
    else if(args[0]==L"--uninstall")command.action=InstallerAction::uninstall;
    else throw std::invalid_argument("Unknown installer action");
    command.request.x64_binary_path=args[1];
    command.request.x86_binary_path=args[2];
    command.request.icon_path=args[3];
    command.request.expected_x64_path=args[4]==L"-"?L"":args[4];
    command.request.expected_x86_path=args[5]==L"-"?L"":args[5];
    for(const auto& path:{command.request.x64_binary_path,command.request.x86_binary_path,command.request.icon_path})
        if(!detail::absolute_path(path))throw std::invalid_argument("Package paths must be absolute");
    for(const auto& path:{command.request.expected_x64_path,command.request.expected_x86_path})
        if(!path.empty()&&!detail::absolute_path(path))throw std::invalid_argument("Expected old paths must be absolute or '-' for absence");
    if(command.action==InstallerAction::uninstall&&
       (command.request.expected_x64_path.empty()||command.request.expected_x86_path.empty()))
        throw std::invalid_argument("Uninstall requires both expected installed DLL paths");
    return command;
}

// '-' permits only a truly absent COM view on a fresh package deployment. It
// never silently adopts an existing registration when no old deployment tokens
// were supplied.
inline HRESULT validate_package_absence(Backend& backend,bool include_x86=true) {
    const Resource resources[]={Resource::com_x64,Resource::com_x86};
    for (size_t i=0;i<(include_x86?2u:1u);++i) {
        const Resource resource=resources[i];
        ResourceState state;
        HRESULT status=backend.read(resource,&state);
        if(status!=S_OK)return status;
        if(state.present)return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    return S_OK;
}

// '-' allows absence or an idempotent retry at this exact product-owned target.
// It never authorizes replacing a different existing DLL path.
inline HRESULT validate_installer_expectations(Backend& backend,const InstallerCommand& command) {
    if(command.action==InstallerAction::status||command.action==InstallerAction::check_deployment_access)return S_OK;
    const Resource resources[]={Resource::com_x64,Resource::com_x86};
    const std::wstring expected[]={command.request.expected_x64_path,command.request.expected_x86_path};
    const std::wstring desired[]={command.request.x64_binary_path,command.request.x86_binary_path};
    const size_t resource_count=command.action==InstallerAction::uninstall_x64?1u:2u;
    for(size_t i=0;i<resource_count;++i) {
        ResourceState current;
        HRESULT status=backend.read(resources[i],&current);
        if(status!=S_OK)return status;
        if(current.present&&expected[i].empty()&&
           (current.owner!=command.request.owner||!detail::same_path(current.path,desired[i])))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    return S_OK;
}

inline LifecycleResult execute_installer_command(Backend& backend,const InstallerCommand& command,
        package_uninstall::Report* removal_report=nullptr,package_categories::Store* categories=nullptr) {
    dev_host_migration_history::Lock lifecycle_lock;
    const bool mutating=command.action!=InstallerAction::status&&command.action!=InstallerAction::check_deployment_access&&
        command.action!=InstallerAction::plan_display&&command.action!=InstallerAction::inspect_dev_host_migration&&
        command.action!=InstallerAction::plan_uninstall_package;
    if(mutating) {
        const HRESULT acquired=lifecycle_lock.acquire();
        if(acquired!=S_OK)return {acquired,S_OK};
    }
    if(command.action==InstallerAction::plan_uninstall_package || command.action==InstallerAction::uninstall_package_registration) {
        package_uninstall::Report ignored;
        return package_uninstall::run(backend,command.deployment.install_root,
            command.action==InstallerAction::uninstall_package_registration,removal_report?removal_report:&ignored,categories);
    }
    if(command.action==InstallerAction::migrate_dev_host)
        return dev_host_package_migration::execute(backend,
            {command.deployment,command.request.expected_x64_path,command.expected_old_dll_sha256});
    // These explicit commands require the fixed-HKLM-key Windows composition,
    // never the generic install/repair or package publication transaction.
    if(command.action==InstallerAction::plan_display || command.action==InstallerAction::repair_display)
        return {E_NOTIMPL,S_OK};
    if(command.action==InstallerAction::check_deployment_access)return {E_NOTIMPL,S_OK};
    if(command.action==InstallerAction::status) {
        std::array<ResourceState,detail::index(Resource::count)> states{};
        return {detail::read_all(backend,states),S_OK};
    }
    if(command.action==InstallerAction::deploy_package) {
        files::detail::Package package;
        if(!files::detail::parse_package(command.deployment.package_root,&package))
            return {HRESULT_FROM_WIN32(ERROR_INVALID_DATA),S_OK};
        if(command.deployment.expected_old_version.empty()) {
            const HRESULT absent=validate_package_absence(backend,package.has_x86);
            if(absent!=S_OK)return {absent,S_OK};
        }
        const DeploymentObserverFactory observer_factory = [&backend](const files::DeploymentRequest& request,
            const files::DeploymentPaths& paths, std::unique_ptr<TransactionObserver>* output) {
            return durable_cli::create_observer(request,paths,output,
                [&backend](Resource resource,const ResourceState& state,ResourceState* projected) {
                    return backend.project_apply_state(resource,state,projected);
                });
        };
        return deploy_and_register(backend,command.deployment,nullptr,observer_factory);
    }
    if(command.action!=InstallerAction::install && command.action!=InstallerAction::repair &&
       command.action!=InstallerAction::uninstall && command.action!=InstallerAction::uninstall_x64)
        return {E_INVALIDARG,S_OK};
    class ExpectedBackend final:public Backend {
        Backend& inner_;const InstallerCommand& command_;
    public:
        ExpectedBackend(Backend& inner,const InstallerCommand& command):inner_(inner),command_(command){}
        HRESULT read(Resource resource,ResourceState* state)override {
            if(!state)return E_POINTER;
            HRESULT result=inner_.read(resource,state);
            if(result!=S_OK||!state->present)return result;
            if(resource!=Resource::com_x64&&resource!=Resource::com_x86)return result;
            if(command_.action==InstallerAction::uninstall_x64&&resource==Resource::com_x86)return result;
            const bool x64=resource==Resource::com_x64;
            const auto& expected=x64?command_.request.expected_x64_path:command_.request.expected_x86_path;
            const auto& desired=x64?command_.request.x64_binary_path:command_.request.x86_binary_path;
            if(expected.empty()&&(state->owner!=command_.request.owner||!detail::same_path(state->path,desired)))
                return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            return result;
        }
        HRESULT apply(Resource resource,const ResourceState& state)override{return inner_.apply(resource,state);}
        HRESULT restore(Resource resource,const ResourceState& state)override{return inner_.restore(resource,state);}
        HRESULT validate_install(const InstallRequest& request)override{return inner_.validate_install(request);}
        HRESULT prepare_apply_targets(TransactionOperation op,const InstallRequest& request,const ResourceStates& before,ResourceStates* desired)override{return inner_.prepare_apply_targets(op,request,before,desired);}

        HRESULT verify_applied(Resource resource,const ResourceState& desired,const ResourceState& observed)const override{return inner_.verify_applied(resource,desired,observed);}
        HRESULT project_apply_state(Resource resource,const ResourceState& state,ResourceState* out)const override{return inner_.project_apply_state(resource,state,out);}
        HRESULT verify_install_health()override{return inner_.verify_install_health();}
        bool failure_may_have_mutated(Resource resource,HRESULT status)const override{return inner_.failure_may_have_mutated(resource,status);}
    } guarded(backend,command);
    HRESULT expected=validate_installer_expectations(guarded,command);
    if(expected!=S_OK)return {expected,S_OK};
    if(command.action==InstallerAction::uninstall_x64&&command.request.include_machine_com_x64) {
        std::unique_ptr<TransactionObserver> observer;
        HRESULT status=durable_cli::Session::create_uninstall(command.request,command.deployment.install_root,&observer,
            [&guarded](Resource resource,const ResourceState& state,ResourceState* projected) {
                return guarded.project_apply_state(resource,state,projected);
            });
        if(status!=S_OK||!observer)return {status!=S_OK?status:E_UNEXPECTED,S_OK};
        auto* session=dynamic_cast<durable_cli::Session*>(observer.get());
        if(!session)return {E_UNEXPECTED,S_OK};
        LifecycleResult result=uninstall(guarded,command.request,observer.get());
        ResourceStates after{};status=detail::read_all(guarded,after);
        if(status!=S_OK)return {result.operation==S_OK?status:result.operation,result.cleanup==S_OK?status:result.cleanup};
        status=session->complete_uninstall(result,after);
        if(status!=S_OK&&result.cleanup==S_OK)result.cleanup=status;
        return result;
    }
    return command.action==InstallerAction::uninstall||command.action==InstallerAction::uninstall_x64
        ?uninstall(guarded,command.request):install_or_repair(guarded,command.request);
}
}
#endif
