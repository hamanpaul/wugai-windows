#pragma once
#ifdef _WIN32
#include "durable_cli_session.hpp"
#include "package_categories.hpp"

namespace boshiamy::installer::package_uninstall {
namespace fs = std::filesystem;
struct File { std::string relative_path, sha256; uint64_t bytes = 0; };
struct Report {
    bool executed = false;
    bool registrations_absent = false;
    bool modern_category_cleanup_started = false;
    std::string failure_stage;
    fs::path root;
    files::DeploymentSnapshot installed;
    std::vector<File> inventory;
    std::string inventory_fingerprint;
};

// Read a complete, known package tree. This deliberately rejects transaction
// directories and unknown files instead of treating them as removable payload.
inline HRESULT inventory(const fs::path& root, Report* report) {
    if (!report) return E_POINTER;
    report->root = root;
    report->inventory.clear(); report->inventory_fingerprint.clear();
    if (!root.is_absolute() || root != root.lexically_normal() || root.filename().empty() ||
        !files::detail::absolute_no_reparse(root, true)) return E_INVALIDARG;
    HRESULT status = files::FileDeployment::inspect(root, "LiuKai-TSF-v1", &report->installed);
    if (status != S_OK) return status;
    if (report->installed.has_x86) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    try {
        const std::set<std::wstring> allowed = {L".liukai-owner", L".liukai-install-state",
            L"LiuKaiInstaller.exe", L"shared", L"versions"};
        std::set<std::wstring> found;
        for (const auto& entry : fs::directory_iterator(root)) {
            const auto name = entry.path().filename().wstring();
            if (!allowed.count(name) || files::detail::reparse(entry.path())) return E_ACCESSDENIED;
            const bool directory = name == L"shared" || name == L"versions";
            if (directory ? !entry.is_directory() : !entry.is_regular_file()) return E_ACCESSDENIED;
            found.insert(name);
        }
        if (found != allowed) return E_ACCESSDENIED;
        size_t shared_count = 0;
        for (const auto& entry : fs::directory_iterator(root / L"shared")) {
            if (entry.path().filename() != L"branding.dll" || !entry.is_regular_file() ||
                files::detail::reparse(entry.path())) return E_ACCESSDENIED;
            ++shared_count;
        }
        if (shared_count != 1) return E_ACCESSDENIED;
        bool current_seen = false;
        for (const auto& entry : fs::directory_iterator(root / L"versions")) {
            const auto version = entry.path().filename().string();
            if (!entry.is_directory() || files::detail::reparse(entry.path()) ||
                !files::detail::validate_installed_version(entry.path(), "LiuKai-TSF-v1", version))
                return E_ACCESSDENIED;
            current_seen = current_seen || version == report->installed.version;
        }
        if (!current_seen) return E_ACCESSDENIED;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (files::detail::reparse(entry.path())) return E_ACCESSDENIED;
            if (entry.is_directory()) continue;
            if (!entry.is_regular_file()) return E_ACCESSDENIED;
            File file;
            file.relative_path = entry.path().lexically_relative(root).generic_string();
            if (!files::detail::safe_relative(file.relative_path)) return E_ACCESSDENIED;
            file.sha256 = files::detail::sha256_file(entry.path());
            if (!files::detail::valid_hash(file.sha256)) return E_ACCESSDENIED;
            file.bytes = fs::file_size(entry.path());
            report->inventory.push_back(std::move(file));
        }
        std::sort(report->inventory.begin(), report->inventory.end(),
            [](const File& left, const File& right) { return left.relative_path < right.relative_path; });
        std::string material = "liukai-uninstall-inventory-v1\n";
        for (const auto& file : report->inventory)
            material += file.relative_path + "\t" + file.sha256 + "\n";
        durable_journal::detail::Digest digest{};
        if (!durable_journal::detail::sha256(material, &digest)) return E_FAIL;
        static constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : digest) {
            report->inventory_fingerprint.push_back(hex[byte >> 4]);
            report->inventory_fingerprint.push_back(hex[byte & 15]);
        }
        return S_OK;
    } catch (...) { return E_ACCESSDENIED; }
}

inline HRESULT owned_registration_prestate(const ResourceStates& state,
        const files::DeploymentSnapshot& installed) {
    for (size_t i = 0; i < state.size(); ++i) {
        const auto resource = static_cast<Resource>(i);
        const auto& current = state[i];
        if (!current.present) {
            if (current != ResourceState{}) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            continue;
        }
        if (resource == Resource::com_x86) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (!current.created_by_lifecycle || current.owner != L"LiuKai-TSF-v1")
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if ((resource == Resource::com_x64 || resource == Resource::machine_com_x64) &&
            (!detail::same_path(current.path, installed.x64_dll.wstring()) || current.threading_model != L"Apartment"))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if (resource == Resource::profile && !detail::same_path(current.path, installed.branding_dll.wstring()))
            return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    return S_OK;
}

// This operation removes registrations only. The caller must independently
// verify absence and this complete inventory before deleting program files.
inline LifecycleResult run(Backend& backend, const fs::path& root, bool execute, Report* report,
        package_categories::Store* categories) {
    if (!report) return {E_POINTER, S_OK};
    *report = {}; report->root = root; report->executed = execute;
    report->failure_stage = "inventory";
    HRESULT status = inventory(root, report);
    if (status != S_OK) return {status, S_OK};
    report->failure_stage = "registration-ownership";
    ResourceStates before{};
    status = detail::read_all(backend, before);
    if (status == S_OK) status = owned_registration_prestate(before, report->installed);
    if (status != S_OK) return {status, S_OK};
    report->failure_stage = "category-ownership";
    if (!categories) return {E_NOTIMPL, S_OK};
    package_categories::Categories category_prestate;
    status = package_categories::capture(*categories, &category_prestate);
    if (status != S_OK) return {status, S_OK};
    for (const auto& row : std::vector<std::pair<Resource, GUID>>{
            {Resource::keyboard_category, GUID_TFCAT_TIP_KEYBOARD},
            {Resource::system_tray_category, package_categories::tray},
            {Resource::ui_element_category, package_categories::ui}})
        if (before[static_cast<size_t>(row.first)].present != package_categories::contains(category_prestate,row.second))
            return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    // Modern-host registration has no lifecycle owner marker of its own.
    // Require the matching, owned profile and both x64 pointers as provenance.
    if (std::any_of(category_prestate.begin(),category_prestate.end(),package_categories::extra))
        for (const Resource resource : {Resource::com_x64, Resource::machine_com_x64, Resource::profile})
            if (!before[static_cast<size_t>(resource)].present) return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), S_OK};
    const auto projection = [&backend](Resource resource, const ResourceState& state, ResourceState* out) {
        return backend.project_apply_state(resource, state, out);
    };
    report->failure_stage = "durable-history";
    status = durable_cli::detail::inspect_machine_history(root, "LiuKai-TSF-v1", projection);
    if (status != S_OK) return {status, S_OK};
    if (!execute) { report->failure_stage.clear(); return {S_OK, S_OK}; }
    InstallRequest request;
    request.owner = L"LiuKai-TSF-v1";
    request.include_x86 = false; request.include_machine_com_x64 = true;
    request.x64_binary_path = request.expected_x64_path = report->installed.x64_dll.wstring();
    request.icon_path = report->installed.branding_dll.wstring();
    durable_cli::detail::MachineScopeLock machine_lock;
    status = machine_lock.acquire();
    if (status != S_OK) return {status, S_OK};
    status = durable_cli::detail::inspect_machine_history(root, "LiuKai-TSF-v1", projection);
    if (status != S_OK) return {status, S_OK};
    report->failure_stage = "pre-mutation-revalidation";
    Report checked;
    status = inventory(root, &checked);
    if (status != S_OK || checked.inventory_fingerprint != report->inventory_fingerprint)
        return {status != S_OK ? status : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    ResourceStates fresh{};
    status = detail::read_all(backend, fresh);
    if (status != S_OK || fresh != before)
        return {status != S_OK ? status : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    package_categories::Categories fresh_categories;
    status = package_categories::capture(*categories, &fresh_categories);
    if (status != S_OK || !package_categories::same(fresh_categories, category_prestate))
        return {status != S_OK ? status : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    report->failure_stage = "modern-host-category-removal";
    report->modern_category_cleanup_started = std::any_of(category_prestate.begin(),category_prestate.end(),package_categories::extra);
    status = package_categories::remove_extras(*categories, category_prestate);
    if (status != S_OK) return {status, S_OK};
    // Modern categories have no lifecycle marker and are deliberately outside
    // the seven-resource snapshot. A later failure retains files and reports
    // partial uninstall progress; it must not claim those capabilities restored.
    report->failure_stage = "registration-removal";
    // Do not create an empty transaction directory during modern-category
    // cleanup: interruption there must remain retryable with intact provenance.
    std::unique_ptr<TransactionObserver> observer;
    status = durable_cli::Session::create_uninstall(request, root, &observer, projection);
    if (status != S_OK || !observer) return {status != S_OK ? status : E_UNEXPECTED, S_OK};
    auto* session = dynamic_cast<durable_cli::Session*>(observer.get());
    if (!session) return {E_UNEXPECTED, S_OK};
    auto result = uninstall(backend, request, observer.get());
    ResourceStates after{};
    status = detail::read_all(backend, after);
    if (status != S_OK) return {result.operation == S_OK ? status : result.operation,
        result.cleanup == S_OK ? status : result.cleanup};
    report->registrations_absent = after == ResourceStates{};
    if (result.operation == S_OK && result.cleanup == S_OK && !report->registrations_absent)
        result.operation = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    status = session->complete_uninstall(result, after);
    if (status != S_OK && result.cleanup == S_OK) result.cleanup = status;
    if (result.operation != S_OK || result.cleanup != S_OK) return result;
    report->failure_stage = "all-category-readback";
    report->registrations_absent = false;
    status = package_categories::capture(*categories, &fresh_categories);
    if (status != S_OK) return {status, S_OK};
    if (!fresh_categories.empty()) return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    report->registrations_absent = true;
    report->failure_stage = "post-removal-inventory";
    status = inventory(root, &checked);
    if (status != S_OK || checked.inventory_fingerprint != report->inventory_fingerprint)
        return {status != S_OK ? status : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    report->failure_stage.clear();
    return {S_OK, S_OK};
}
} // namespace boshiamy::installer::package_uninstall
#endif
