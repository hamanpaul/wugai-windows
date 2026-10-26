#pragma once

#include "dual_architecture_transaction.hpp"
#include "file_deployment.hpp"

#include <memory>
#include <string>
#include <system_error>
#include <functional>

namespace boshiamy::installer {

using DeploymentObserverFactory = std::function<HRESULT(
    const files::DeploymentRequest&, const files::DeploymentPaths&,
    std::unique_ptr<TransactionObserver>*)>;
using RegistrationExecutor = std::function<LifecycleResult(Backend&, const InstallRequest&, TransactionObserver*)>;

// Optional extension used by durable package sessions. It brackets the file
// publication phase with the same transaction identity before TSF mutation
// begins. Legacy observers remain unchanged.
class FileDeploymentObserver {
public:
    virtual ~FileDeploymentObserver() = default;
    virtual std::string file_transaction_token() const = 0;
    virtual HRESULT before_file_publication(const ResourceStates& pre_state,
        const files::DeploymentSnapshot* old_files, const files::DeploymentPaths& desired_files) = 0;
    virtual HRESULT after_file_publication(HRESULT operation_status,
        const files::DeploymentSnapshot* observed_files) = 0;
    virtual HRESULT before_file_rollback() = 0;
    virtual HRESULT after_file_rollback(HRESULT operation_status,
        const files::DeploymentSnapshot* observed_files) = 0;
    virtual HRESULT complete_failed_compensation(const ResourceStates&,
        const files::DeploymentSnapshot*) { return S_OK; }
};

// Joins the offline file transaction to the COM/TSF transaction. This layer
// never owns registry or TSF APIs; it keeps versioned DLLs available until
// registration has committed and preserves them if registration rollback is
// incomplete.
inline LifecycleResult deploy_and_register(Backend& backend,
        const files::DeploymentRequest& file_request, TransactionObserver* observer=nullptr,
        const DeploymentObserverFactory& observer_factory={},
        const RegistrationExecutor& registration_executor={}) {
    using files::DeploymentSnapshot;
    using files::FileDeployment;
    using files::PreparedDeployment;
    namespace fs = std::filesystem;

    DeploymentSnapshot old_files;
    std::error_code ec;
    bool root_exists = false;
    bool version_preexisted = false;
    bool file_apply_started = false;
    files::DeploymentPaths paths;
    std::unique_ptr<PreparedDeployment> files_transaction;
    std::unique_ptr<TransactionObserver> owned_observer;
    TransactionObserver* effective_observer = observer;
    FileDeploymentObserver* file_observer = nullptr;
    InstallRequest registration;
    HRESULT status = E_FAIL;
    try {
        root_exists = fs::exists(file_request.install_root, ec);
        if (ec) return {HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value())), S_OK};
        if (root_exists) {
            const HRESULT inspected = FileDeployment::inspect(file_request.install_root,
                                                              file_request.owner, &old_files);
            if (FAILED(inspected)) return {inspected, S_OK};
            if (old_files.version != file_request.expected_old_version ||
                old_files.installer_cli_sha256 != file_request.expected_old_cli_sha256 ||
                old_files.branding_sha256 != file_request.expected_old_branding_sha256)
                return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
        } else if (!file_request.expected_old_version.empty() ||
                   !file_request.expected_old_cli_sha256.empty() ||
                   !file_request.expected_old_branding_sha256.empty()) {
            return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
        }
        status = FileDeployment::prepare(file_request, &files_transaction);
        if (FAILED(status) || !files_transaction)
            return {FAILED(status) ? status : E_UNEXPECTED, S_OK};
        paths = files_transaction->paths();
        registration.owner.assign(file_request.owner.begin(), file_request.owner.end());
        registration.x64_binary_path = paths.x64_dll.wstring();
        registration.x86_binary_path = paths.x86_dll.wstring();
        registration.include_x86 = paths.has_x86;
        registration.include_machine_com_x64 = file_request.include_machine_com_x64;
        registration.icon_path = paths.branding_dll.wstring();
        if (root_exists) {
            registration.expected_x64_path = old_files.x64_dll.wstring();
            registration.expected_x86_path = old_files.x86_dll.wstring();
        }
        // Refuse an incompatible or foreign machine/HKCU pair before file
        // publication. Installation repeats this read before registration writes.
        status = detail::validate_machine_com_policy(registration);
        if (status != S_OK) return {status, S_OK};
        if (registration.include_machine_com_x64) {
            ResourceStates pre_state{};
            status = detail::read_all(backend, pre_state);
            if (status != S_OK) return {status, S_OK};
            status = detail::validate_machine_com_prestate(registration, pre_state);
            if (status != S_OK) return {status, S_OK};
            for (Resource resource : {Resource::profile, Resource::keyboard_category, Resource::system_tray_category, Resource::ui_element_category}) {
                const auto& current = pre_state[detail::index(resource)];
                if (current.present && current.owner != registration.owner)
                    return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), S_OK};
            }
        }
        if (observer && observer_factory) return {E_INVALIDARG, S_OK};
        if (observer_factory) {
            status = observer_factory(file_request, paths, &owned_observer);
            if (status != S_OK || !owned_observer)
                return {status != S_OK ? status : E_UNEXPECTED, S_OK};
            effective_observer = owned_observer.get();
        }
        file_observer = dynamic_cast<FileDeploymentObserver*>(effective_observer);
        if (file_observer) {
            const std::string token = file_observer->file_transaction_token();
            status = files_transaction->bind_transaction_token(token);
            if (status != S_OK) return {status, S_OK};
            ResourceStates pre_state{};
            status = detail::read_all(backend, pre_state);
            if (status != S_OK) return {status, S_OK};
            status = file_observer->before_file_publication(pre_state,
                root_exists ? &old_files : nullptr, paths);
            if (status != S_OK) return {status, S_OK};
        }
        version_preexisted = fs::exists(paths.version_root, ec);
        if (ec) return {HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value())), S_OK};
        file_apply_started = true;
        status = files_transaction->apply();
        if (FAILED(status)) {
            if (file_observer) {
                DeploymentSnapshot observed_files;
                const bool observed_exists = fs::exists(file_request.install_root, ec) && !ec;
                HRESULT observation_status = S_OK;
                const files::DeploymentSnapshot* observed_ptr = nullptr;
                if (observed_exists) {
                    observation_status = FileDeployment::inspect(file_request.install_root,
                        file_request.owner, &observed_files);
                    if (SUCCEEDED(observation_status)) observed_ptr = &observed_files;
                }
                if (FAILED(observation_status))
                    return {status, E_ACCESSDENIED};
                const HRESULT journal_status = file_observer->after_file_publication(status, observed_ptr);
                if (FAILED(journal_status)) return {status, E_ACCESSDENIED};
            }
            bool pending = false;
            ec.clear();
            if (fs::exists(file_request.install_root, ec)) {
                if (ec) pending = true;
                else {
                    std::error_code iterate_error;
                    for (fs::directory_iterator it(file_request.install_root, iterate_error), end;
                         it != end && !iterate_error; it.increment(iterate_error)) {
                        if (it->path().filename().wstring().rfind(L".txn-", 0) == 0) { pending = true; break; }
                    }
                    if (iterate_error) pending = true;
                }
            } else if (ec) pending = true;
            ec.clear();
            if (!version_preexisted && fs::exists(paths.version_root, ec)) pending = true;
            if (ec) pending = true;
            // FileDeployment::apply reports its failure HRESULT, but a retained
            // transaction or an unexpectedly published version means the
            // caller must reconcile instead of assuming cleanup completed.
            return {status, pending ? E_ACCESSDENIED : S_OK};
        }
        if (file_observer) {
            DeploymentSnapshot published;
            status = FileDeployment::inspect(file_request.install_root, file_request.owner, &published);
            if (status != S_OK) return {status, E_ACCESSDENIED};
            status = file_observer->after_file_publication(S_OK, &published);
            if (FAILED(status)) return {status, E_ACCESSDENIED};
        }
    } catch (...) {
        // Once apply begins, an exception during apply or pending-artifact
        // inspection leaves cleanup state unknown. Never report it as clean.
        return {E_FAIL, file_apply_started ? E_ACCESSDENIED : S_OK};
    }

    LifecycleResult registration_result;
    try {
        registration_result = registration_executor
            ?registration_executor(backend, registration, effective_observer)
            :install_or_repair(backend, registration, effective_observer);
    } catch (...) {
        // A throwing backend has unknown mutation state. Keep the published
        // version and its transaction journal so a possibly-live COM reference
        // cannot be left pointing at files we removed.
        return {E_UNEXPECTED, E_UNEXPECTED};
    }

    if (registration_result.operation != S_OK || registration_result.cleanup != S_OK) {
        // A failed TSF rollback means registrations may still reference the new
        // DLLs. Preserve those files and the journal for explicit recovery.
        if (registration_result.cleanup != S_OK) return registration_result;

        HRESULT file_cleanup = E_FAIL;
        HRESULT before_rollback = S_OK;
        try {
            if (file_observer) before_rollback = file_observer->before_file_rollback();
            if (before_rollback != S_OK) return {registration_result.operation, E_ACCESSDENIED};
            file_cleanup = files_transaction->rollback();
            if (file_observer) {
                DeploymentSnapshot restored;
                const bool restored_exists = fs::exists(file_request.install_root, ec) && !ec;
                const files::DeploymentSnapshot* restored_ptr = nullptr;
                HRESULT inspect_status = S_OK;
                if (restored_exists) {
                    inspect_status = FileDeployment::inspect(file_request.install_root,
                        file_request.owner, &restored);
                    if (SUCCEEDED(inspect_status)) restored_ptr = &restored;
                }
                if (FAILED(inspect_status)) file_cleanup = E_ACCESSDENIED;
                else {
                    const HRESULT journal_status = file_observer->after_file_rollback(file_cleanup, restored_ptr);
                    if (journal_status != S_OK) file_cleanup = E_ACCESSDENIED;
                    else if (file_cleanup == S_OK) {
                        ResourceStates fresh{};
                        const HRESULT read_status=detail::read_all(backend,fresh);
                        if (read_status!=S_OK) file_cleanup=read_status;
                        else {
                            const HRESULT complete=file_observer->complete_failed_compensation(fresh,restored_ptr);
                            if (complete!=S_OK) file_cleanup=complete;
                        }
                    }
                }
            }
        }
        catch (...) { file_cleanup = E_UNEXPECTED; }
        return {registration_result.operation, file_cleanup};
    }

    if(effective_observer) {
        try {
            ResourceStates final_state{};
            const HRESULT read_status=detail::read_all(backend,final_state);
            if(read_status!=S_OK)return {read_status,E_ACCESSDENIED};
            const HRESULT commit_status=effective_observer->commit(final_state);
            // An uncommitted journal still owns recovery evidence; retain the files.
            if(commit_status!=S_OK)return {commit_status,E_ACCESSDENIED};
        } catch(...) {
            return {E_UNEXPECTED,E_ACCESSDENIED};
        }
    }

    HRESULT finalized = E_FAIL;
    try { finalized = files_transaction->finalize(); }
    catch (...) { finalized = E_UNEXPECTED; }
    // A finalize error is cleanup-only: both file and TSF state have committed,
    // and the deployment journal remains for recovery.
    return {S_OK, finalized};
}

} // namespace boshiamy::installer
