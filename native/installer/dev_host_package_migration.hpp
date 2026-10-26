#pragma once
#include "deployment_coordinator.hpp"
#include "durable_cli_session.hpp"
#include "dev_host_migration.hpp"
#include "dev_host_migration_history.hpp"
#include "profile_display_payload.hpp"
#include "profile_display_plan.hpp"

namespace boshiamy::installer::dev_host_package_migration {
namespace fs = std::filesystem;
struct Request {
    files::DeploymentRequest deployment;
    std::wstring expected_old_path;
    std::string expected_old_sha256;
};
struct SourceLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~SourceLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
inline LifecycleResult execute(Backend& backend, const Request& request) {
    const auto& deployment = request.deployment;
    if (deployment.include_machine_com_x64 || deployment.owner != "LiuKai-TSF-v1" || !deployment.install_root.is_absolute() ||
        !deployment.package_root.is_absolute() || !deployment.expected_old_version.empty() ||
        !deployment.expected_old_cli_sha256.empty() || !deployment.expected_old_branding_sha256.empty() ||
        !profile_display_plan::detail::valid_stable_module(request.expected_old_path) ||
        !files::detail::valid_hash(request.expected_old_sha256)) return {E_INVALIDARG, S_OK};
    const fs::path source = request.expected_old_path;
    if (!files::detail::absolute_no_reparse(source, true) ||
        files::detail::is_within(deployment.install_root, source) ||
        files::detail::is_within(deployment.install_root, deployment.package_root)) return {E_INVALIDARG, S_OK};
    std::error_code error;
    if (fs::exists(deployment.install_root, error) || error)
        return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), S_OK};
    files::detail::Package package;
    if (!files::detail::parse_package(deployment.package_root, &package) || package.has_x86 ||
        package.files.at(files::detail::version_prefix(package.version, "x64") + "BoshiamyTsf.dll").hash != request.expected_old_sha256)
        return {HRESULT_FROM_WIN32(ERROR_INVALID_DATA), S_OK};
    SourceLock source_lock;
    source_lock.handle = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source_lock.handle == INVALID_HANDLE_VALUE) return {HRESULT_FROM_WIN32(GetLastError()), S_OK};
    const auto source_valid = [&]() -> HRESULT {
        return files::detail::absolute_no_reparse(source, true) && files::detail::architecture(source, true, true) &&
            files::detail::sha256_file(source) == request.expected_old_sha256
            ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    };
    HRESULT status = source_valid();
    if (status != S_OK) return {status, S_OK};
    dev_host_migration_history::Lock transaction_lock;
    status = transaction_lock.acquire();
    if (status != S_OK) return {status, S_OK};
    status = dev_host_migration_history::inspect(deployment.install_root);
    if (status != S_OK) return {status, S_OK};
    ResourceStates before{};
    status = installer::detail::read_all(backend, before);
    if (status != S_OK) return {status, S_OK};
    const auto& com = before[static_cast<size_t>(Resource::com_x64)];
    const auto& profile = before[static_cast<size_t>(Resource::profile)];
    if (!com.present || !com.created_by_lifecycle || com.owner != L"LiuKai-TSF-v1" ||
        com.threading_model != L"Apartment" || !profile.present ||
        !profile_display::resource_name::exact_path_equal(com.path, request.expected_old_path))
        return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    profile_display_payload::Payload full_profile;
    status = profile_display_payload::decode(profile.private_data, &full_profile);
    if (status != S_OK || !full_profile.has_display || !profile_display_plan::detail::exact_identity(full_profile.display))
        return {status == S_OK ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : status, S_OK};
    const DeploymentObserverFactory factory = [&](const files::DeploymentRequest& input,
        const files::DeploymentPaths& paths, std::unique_ptr<TransactionObserver>* output) {
        ResourceStates current{};
        HRESULT checked = source_valid();
        if (checked != S_OK) return checked;
        checked = installer::detail::read_all(backend, current);
        if (checked != S_OK) return checked;
        if (current != before) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return durable_cli::create_observer(input, paths, output,
            [&](Resource resource, const ResourceState& value, ResourceState* projected) {
                return backend.project_apply_state(resource, value, projected);
            }, TransactionOperation::migrate_x64_dev_host);
    };
    const RegistrationExecutor executor = [&](Backend& target, const InstallRequest& registration,
                                               TransactionObserver* observer) -> LifecycleResult {
        ResourceStates current{};
        const HRESULT observed = installer::detail::read_all(target, current);
        if (observed != S_OK) return {observed, E_ACCESSDENIED};
        if (current != before) return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), E_ACCESSDENIED};
        const HRESULT checked = source_valid();
        if (checked != S_OK) return {checked, E_ACCESSDENIED};
        InstallRequest migration = registration;
        migration.expected_x64_path = request.expected_old_path;
        return dev_host_migration::execute(target, migration, before, observer);
    };
    return deploy_and_register(backend, deployment, nullptr, factory, executor);
}
}
