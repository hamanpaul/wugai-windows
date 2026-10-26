#pragma once
#include "command_line.hpp"
#include "profile_display_repair.hpp"
#include "windows_profile_display_access.hpp"
#include "profile_display_health.hpp"
#include "tsf_shared_store.hpp"
#include <memory>

namespace boshiamy::installer::profile_display_repair_windows {
namespace fs = std::filesystem;
namespace repair = profile_display_repair;
using Snapshot = repair::Snapshot;
struct Report {
    LifecycleResult result{E_FAIL, S_OK};
    bool read_only = true, ready = false, write_access_opened = false, repair_started = false;
    std::wstring module, ledger;
    std::string before_sha256, target_sha256, branding_sha256;
};
namespace detail {
inline constexpr char kLedgerOwner[] = "LiuKai-Display-v1";
inline fs::path ledger_base(const fs::path& root) {
    return root.lexically_normal().parent_path() / L".liukai-installer-ledger" /
        files::detail::widen_ascii(kLedgerOwner);
}
inline HRESULT inspect_prior_repairs(const fs::path& root) {
    try {
        const auto base = ledger_base(root);
        const HRESULT uncertain = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::error_code error;
        if (!fs::exists(base, error)) return error ? uncertain : S_OK;
        if (error || !files::detail::absolute_no_reparse(base, true) || !fs::is_directory(base)) return uncertain;
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(base)) {
            if (++count > 1024 || !files::detail::absolute_no_reparse(entry.path(), true) || !entry.is_directory())
                return uncertain;
            std::string image;
            if (!durable_journal::detail::read_file(entry.path() / L"transaction.journal", &image)) return uncertain;
            size_t at = 0;
            durable_journal::Identity identity;
            durable_journal::detail::Digest chain{};
            if (!durable_journal::detail::parse_header(image, &at, &identity, &chain) ||
                identity.schema_version != 2 || identity.owner != kLedgerOwner ||
                !durable_journal::detail::same_path(identity.storage_root, entry.path().wstring())) return uncertain;
            durable_journal::Store journal(entry.path() / L"transaction.journal", identity, entry.path());
            durable_journal::Document document;
            if (journal.read(identity, &document) != S_OK || !document.committed || !document.all_operations_succeeded ||
                document.events.size() < 3 || document.events.front().kind != durable_journal::Event::Kind::intent ||
                document.events.front().intent.resource != "prestate" ||
                document.events.front().intent.action != "capture-profile-display-repair" ||
                document.events.front().intent.expected_before_fingerprint != std::string(64, '0')) return uncertain;
            for (const auto& event : document.events)
                if (event.kind == durable_journal::Event::Kind::observation &&
                    event.observation.operation_status != S_OK) return uncertain;
            durable_snapshot::Store snapshot(entry.path() / L"prestate.snapshot", identity, entry.path());
            ResourceStates original{};
            std::string fingerprint;
            if (snapshot.read(identity, &original, &fingerprint) != S_OK ||
                document.events.front().intent.desired_fingerprint != fingerprint) return uncertain;
            for (size_t i = 2; i + 1 < document.events.size(); i += 2) {
                const auto& event = document.events[i];
                if (event.kind != durable_journal::Event::Kind::intent || event.intent.resource != "profile" ||
                    (event.intent.action != "apply" && event.intent.action != "restore")) return uncertain;
            }
        }
        return S_OK;
    } catch (...) { return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
}
struct RepairLock {
    HANDLE value = nullptr;
    bool owned = false;
    ~RepairLock() { if (owned) ReleaseMutex(value); if (value) CloseHandle(value); }
    HRESULT acquire(const std::wstring& name) {
        value = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!value) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD waited = WaitForSingleObject(value, 0);
        if (waited == WAIT_ABANDONED) { owned = true; return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
        if (waited == WAIT_OBJECT_0) { owned = true; return S_OK; }
        return waited == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY) : HRESULT_FROM_WIN32(GetLastError());
    }
};
struct Key {
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
    HRESULT open_profile(REGSAM view, bool writable) {
        const LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            profile_display::windows_detail::profile_key_path().c_str(), 0,
            KEY_QUERY_VALUE | (writable ? KEY_SET_VALUE : 0) | view, &value);
        return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
    }
};
inline bool same_package(const files::DeploymentSnapshot& a, const files::DeploymentSnapshot& b) {
    return a.version == b.version && a.manifest_sha256 == b.manifest_sha256 &&
        a.installer_cli_sha256 == b.installer_cli_sha256 && a.branding_sha256 == b.branding_sha256 &&
        a.x64_dll == b.x64_dll && a.x86_dll == b.x86_dll && a.has_x86 == b.has_x86 &&
        a.branding_dll == b.branding_dll && a.installer_cli == b.installer_cli;
}
inline bool attributable(const Snapshot& value, const Snapshot& before, const Snapshot& target) {
    if (!profile_display::detail::same_except_descriptions(value.profile, before.profile)) return false;
    const auto known = [](const auto& a, const auto& b, const auto& c) { return a == b || a == c; };
    const auto raw_known = [](const auto& a, const auto& b, const auto& c) {
        return profile_display::resource_name::same_value(a, b) || profile_display::resource_name::same_value(a, c);
    };
    return known(value.profile.api_description, before.profile.api_description, target.profile.api_description) &&
        known(value.profile.registry.description, before.profile.registry.description, target.profile.registry.description) &&
        raw_known(value.descriptions.registry64, before.descriptions.registry64, target.descriptions.registry64) &&
        raw_known(value.descriptions.registry32, before.descriptions.registry32, target.descriptions.registry32) &&
        raw_known(value.display_binding.registry64, before.display_binding.registry64, target.display_binding.registry64) &&
        raw_known(value.display_binding.registry32, before.display_binding.registry32, target.display_binding.registry32);
}
inline HRESULT open_registration(WindowsTsfSharedStore& shared, std::unique_ptr<WindowsInstallerBackend>* out) {
    HKEY view64 = nullptr, view32 = nullptr;
    HRESULT status = WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER, L"Software\\Classes",
        Resource::com_x64, KEY_READ, &view64);
    if (status != S_OK) return status;
    status = WindowsInstallerBackend::open_classes_root(HKEY_CURRENT_USER, L"Software\\Classes",
        Resource::com_x86, KEY_READ, &view32);
    if (status != S_OK) { RegCloseKey(view64); return status; }
    auto* backend = new (std::nothrow) WindowsInstallerBackend(view64, view32, kInstallerTsfClsid, L"LiuKai-TSF-v1", shared);
    if (!backend) { RegCloseKey(view64); RegCloseKey(view32); return E_OUTOFMEMORY; }
    out->reset(backend);
    return backend->ready();
}
}

// This is the only product-key opener for this command. Query/set access is
// requested only for the fixed HKLM product profile; it never elevates, creates
// a profile/owner marker, publishes files, or falls back to another hive.
inline Report execute(const InstallerCommand& command) {
    Report report;
    const bool applying = command.action == InstallerAction::repair_display;
    report.read_only = !applying;
    const auto fail = [&](HRESULT status) { report.result = {status, S_OK}; return report; };
    if (!applying && command.action != InstallerAction::plan_display) return fail(E_INVALIDARG);
    const auto& root = command.deployment.install_root;
    if (!root.is_absolute() || (applying &&
        (!files::detail::valid_hash(command.expected_display_sha256) ||
         !files::detail::valid_hash(command.expected_branding_sha256)))) return fail(E_INVALIDARG);
    files::DeploymentSnapshot installed;
    HRESULT status = files::FileDeployment::inspect(root, "LiuKai-TSF-v1", &installed);
    if (status != S_OK) return fail(status);
    if (installed.has_x86 || installed.branding_dll != root / L"shared" / L"branding.dll")
        return fail(E_INVALIDARG);
    report.module = installed.branding_dll.wstring();
    report.branding_sha256 = installed.branding_sha256;
    if (applying && command.expected_branding_sha256 != installed.branding_sha256)
        return fail(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    WindowsTsfSharedApi api;
    if (api.ready() != S_OK) return fail(api.ready());
    RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER, L"Software\\LiuKai\\Installer", kInstallerTsfClsid, L"LiuKai-TSF-v1");
    WindowsTsfSharedStore shared(api, markers, L"LiuKai-TSF-v1");
    std::unique_ptr<WindowsInstallerBackend> registration;
    status = detail::open_registration(shared, &registration);
    if (status != S_OK) return fail(status);
    ResourceStates original_registration{};
    status = installer::detail::read_all(*registration, original_registration);
    if (status != S_OK) return fail(status);
    const auto& com = original_registration[static_cast<size_t>(Resource::com_x64)];
    if (!com.present || !com.created_by_lifecycle || com.owner != L"LiuKai-TSF-v1" ||
        com.threading_model != L"Apartment" ||
        !profile_display::resource_name::exact_path_equal(com.path, installed.x64_dll.wstring()))
        return fail(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    profile_display::WindowsProfileDisplayBackend profiles;
    if (profiles.ready() != S_OK) return fail(profiles.ready());
    detail::Key query64, query32;
    status = query64.open_profile(KEY_WOW64_64KEY, false);
    if (status != S_OK) return fail(status);
    status = query32.open_profile(KEY_WOW64_32KEY, false);
    if (status != S_OK) return fail(status);
    const std::wstring mutex = L"Global\\LiuKai-7254A21F-ProfileDisplayMetadata";
    // CTF TIP keys are shared across WOW64 views. Never infer aliasing from data.
    profile_display_windows::WindowsDisplayAccess query(profiles, query64.value, query32.value, true, mutex);
    Snapshot before, target;
    status = query.capture(&before);
    if (status != S_OK) return fail(status);
    status = profile_display_plan::make_target(before, report.module, &target);
    if (status != S_OK) return fail(status);
    status = repair::fingerprint(before, &report.before_sha256);
    if (status != S_OK) return fail(status);
    status = repair::fingerprint(target, &report.target_sha256);
    if (status != S_OK) return fail(status);
    status = profile_display_health::verify(target, report.module, installed.branding_sha256);
    if (status != S_OK) return fail(status);
    const auto unchanged = [&]() -> HRESULT {
        files::DeploymentSnapshot current;
        HRESULT observed = files::FileDeployment::inspect(root, "LiuKai-TSF-v1", &current);
        if (observed != S_OK) return observed;
        if (!detail::same_package(current, installed)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        ResourceStates state{};
        observed = installer::detail::read_all(*registration, state);
        if (observed != S_OK) return observed;
        return state == original_registration ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    };
    status = unchanged();
    if (status != S_OK) return fail(status);
    Snapshot rechecked;
    status = query.capture(&rechecked);
    if (status != S_OK) return fail(status);
    if (!profile_display_state::equal(before, rechecked)) return fail(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    report.ready = true;
    if (!applying) { report.result = {S_OK, S_OK}; return report; }
    if (command.expected_display_sha256 != report.before_sha256)
        return fail(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    // Hold the same reentrant Windows mutex across inventory, durable intent,
    // all per-value exchanges and commit. A second command cannot create a
    // competing repair while an earlier intent is incomplete.
    detail::RepairLock repair_lock;
    status = repair_lock.acquire(mutex);
    if (status != S_OK) return fail(status);
    status = detail::inspect_prior_repairs(root);
    if (status != S_OK) return fail(status);
    detail::Key write64, write32;
    status = write64.open_profile(KEY_WOW64_64KEY, true);
    if (status != S_OK) return fail(status);
    status = write32.open_profile(KEY_WOW64_32KEY, true);
    if (status != S_OK) return fail(status);
    report.write_access_opened = true;
    const auto authority = [&](const Snapshot& expected, const Snapshot& wanted) -> HRESULT {
        if (!detail::attributable(expected, before, target) || !detail::attributable(wanted, before, target))
            return E_ACCESSDENIED;
        return unchanged();
    };
    const auto health = [&](const Snapshot& wanted) {
        return profile_display_health::verify(wanted, report.module, installed.branding_sha256);
    };
    profile_display_windows::WindowsDisplayAccess access(profiles, write64.value, write32.value, true, mutex, authority, health);
    repair::RegistrationView view(*registration);
    profile_display_backend::DisplayBackend backend(view, access,
        profile_display_backend::Mode::existing_profile_metadata, report.module);
    // A dedicated durable token and capture action distinguish this repair from
    // deployment. Original complete states remain available for cold recovery.
    durable_journal::Identity identity;
    identity.schema_version = 2; identity.owner = detail::kLedgerOwner;
    identity.install_root = root.lexically_normal().wstring();
    identity.package_version = installed.version; identity.package_manifest_sha256 = installed.manifest_sha256;
    if (!durable_cli::detail::make_token(&identity.token)) return fail(E_FAIL);
    const auto ledger = detail::ledger_base(root) / files::detail::widen_ascii(identity.token);
    if (!durable_cli::detail::ensure_directory_chain(ledger.parent_path()) ||
        !CreateDirectoryW(ledger.c_str(), nullptr)) return fail(E_ACCESSDENIED);
    identity.storage_root = ledger.wstring(); report.ledger = ledger.wstring();
    if (!durable_journal::detail::valid_identity(identity)) return fail(E_INVALIDARG);
    durable_journal::Store journal(ledger / L"transaction.journal", identity, ledger);
    durable_snapshot::Store snapshot(ledger / L"prestate.snapshot", identity, ledger);
    durable_observer::Observer observer(journal, snapshot, identity,
        [&](Resource resource, const ResourceState& value, ResourceState* projected) {
            return backend.project_apply_state(resource, value, projected);
        });
    InstallRequest request;
    request.owner = L"LiuKai-TSF-v1"; request.include_x86 = false;
    request.x64_binary_path = installed.x64_dll.wstring(); request.expected_x64_path = request.x64_binary_path;
    request.icon_path = report.module;
    report.repair_started = true;
    report.result = repair::execute(backend, request, command.expected_display_sha256, observer);
    return report;
}
}
