#include "../installer/command_line.hpp"
#include "../installer/windows_backend.hpp"
#include "../installer/profile_display_backend.hpp"
#include "../installer/dev_host_migration_recovery.hpp"
#include <fstream>
#include <iostream>

using namespace boshiamy::installer;
namespace fs = std::filesystem;

#define CHECK(x) do { if (!(x)) { std::cerr << "CHECK failed: " #x " line " << __LINE__ << '\n'; return false; } } while (0)

struct RegistryScope {
    std::wstring path;
    HKEY view64 = nullptr;
    HKEY view32 = nullptr;
    bool create() {
        GUID id{}; wchar_t name[40]{};
        if (CoCreateGuid(&id) != S_OK || StringFromGUID2(id, name, 40) != 39) return false;
        path = L"Software\\LiuKaiTests\\ProgramFilesMigration\\" + std::wstring(name);
        const LONG a = RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\64").c_str(), 0, nullptr, 0,
                                       KEY_ALL_ACCESS, nullptr, &view64, nullptr);
        const LONG b = RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\32").c_str(), 0, nullptr, 0,
                                       KEY_ALL_ACCESS, nullptr, &view32, nullptr);
        return a == ERROR_SUCCESS && b == ERROR_SUCCESS;
    }
    ~RegistryScope() {
        const std::wstring prefix = L"Software\\LiuKaiTests\\ProgramFilesMigration\\";
        if (path.size() == prefix.size() + 38 && path.compare(0, prefix.size(), prefix) == 0)
            RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};

struct Shared final : SharedResourceStore {
    ResourceState values[4]{};
    unsigned writes = 0, health = 0;
    HRESULT read(Resource resource, ResourceState* out) override {
        if (!out) return E_POINTER;
        switch (resource) {
        case Resource::profile: *out = values[0]; break;
        case Resource::keyboard_category: *out = values[1]; break;
        case Resource::system_tray_category: *out = values[2]; break;
        case Resource::ui_element_category: *out = values[3]; break;
        default: return E_INVALIDARG;
        }
        return S_OK;
    }
    HRESULT apply(Resource, const ResourceState&) override { ++writes; return E_FAIL; }
    HRESULT restore(Resource, const ResourceState&) override { ++writes; return E_FAIL; }
    HRESULT verify_install_health() override { ++health; return E_FAIL; }
};

struct Display final : profile_display_backend::DisplayAccess {
    profile_display_state::DisplaySnapshot value;
    unsigned writes = 0;
    HRESULT capture(profile_display_state::DisplaySnapshot* out) override { *out = value; return S_OK; }
    HRESULT authorize(const profile_display_state::DisplaySnapshot&,
                      const profile_display_state::DisplaySnapshot&) override { return E_ACCESSDENIED; }
    HRESULT exchange(const profile_display_state::DisplaySnapshot&,
                     const profile_display_state::DisplaySnapshot&) override { ++writes; return E_FAIL; }
    HRESULT verify_target(const profile_display_state::DisplaySnapshot&) override { ++writes; return E_FAIL; }
};

struct LateFailureBackend final : Backend {
    WindowsInstallerBackend& inner;
    bool injected = false;
    explicit LateFailureBackend(WindowsInstallerBackend& value) : inner(value) {}
    HRESULT read(Resource resource, ResourceState* out) override { return inner.read(resource, out); }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        const HRESULT status = inner.apply(resource, desired);
        if (resource == Resource::com_x64 && status == S_OK && !injected) {
            injected = true;
            return E_FAIL; // The sandbox registry write happened; force guarded compensation.
        }
        return status;
    }
    HRESULT restore(Resource resource, const ResourceState& snapshot) override { return inner.restore(resource, snapshot); }
    HRESULT verify_applied(Resource resource, const ResourceState& desired,
                           const ResourceState& observed) const override {
        return inner.verify_applied(resource, desired, observed);
    }
    HRESULT project_apply_state(Resource resource, const ResourceState& state,
                                ResourceState* out) const override {
        return inner.project_apply_state(resource, state, out);
    }
    HRESULT verify_install_health() override { return inner.verify_install_health(); }
    HRESULT validate_install(const InstallRequest& request) override { return inner.validate_install(request); }
    HRESULT prepare_apply_targets(TransactionOperation operation, const InstallRequest& request,
                                  const ResourceStates& before, ResourceStates* desired) override {
        return inner.prepare_apply_targets(operation, request, before, desired);
    }
    bool failure_may_have_mutated(Resource resource, HRESULT status) const override {
        return resource == Resource::com_x64 && status == E_FAIL && injected
            ? true : inner.failure_may_have_mutated(resource, status);
    }
};

bool write_string(HKEY parent, const std::wstring& path, const wchar_t* name, const std::wstring& text) {
    HKEY key = nullptr;
    CHECK(RegCreateKeyExW(parent, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
    const LONG result = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()), static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    CHECK(result == ERROR_SUCCESS);
    return true;
}

bool run_case(const fs::path& package, const fs::path& sandbox, unsigned kind) {
    CHECK(fs::create_directory(sandbox));
    files::detail::Package parsed;
    CHECK(files::detail::parse_package(package, &parsed) && !parsed.has_x86);
    const fs::path old_dll = sandbox / L"old source.dll";
    CHECK(fs::copy_file(package / L"versions" / files::detail::widen_ascii(parsed.version) /
                        L"x64" / L"BoshiamyTsf.dll", old_dll));
    const std::string source_hash = files::detail::sha256_file(old_dll);
    RegistryScope registry;
    CHECK(registry.create());
    wchar_t clsid[40]{};
    CHECK(StringFromGUID2(boshiamy::kServiceClsid, clsid, 40) == 39);
    const std::wstring class_key = L"CLSID\\" + std::wstring(clsid);
    CHECK(write_string(registry.view32, class_key, L"LiuKaiOwner", L"Foreign-x86"));
    CHECK(write_string(registry.view32, class_key + L"\\InprocServer32", nullptr, L"C:\\foreign\\retained.dll"));
    CHECK(write_string(registry.view32, class_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment"));

    Shared shared;
    shared.values[0] = {true, false, L"LiuKai-TSF-v1", L"", L"", {0x13, 0x37}};
    shared.values[1] = {true, false, L"LiuKai-TSF-v1", L"", L"", {7}};
    WindowsInstallerBackend registry_backend(registry.view64, registry.view32, boshiamy::kServiceClsid,
                                               L"LiuKai-TSF-v1", shared);
    ResourceState empty;
    CHECK(registry_backend.read(Resource::com_x64, &empty) == S_OK);
    const ResourceState initial{true, true, L"LiuKai-TSF-v1", old_dll.wstring(), L"Apartment", {}};
    CHECK(registry_backend.apply(Resource::com_x64, initial) == S_OK);
    CHECK(write_string(registry.view64, class_key + L"\\PreserveUnknown", L"value", L"keep me"));

    Display display;
    auto& profile = display.value.profile;
    profile.profile_type = TF_PROFILETYPE_INPUTPROCESSOR;
    profile.language = boshiamy::kProfileLanguage;
    profile.service = boshiamy::kServiceClsid;
    profile.profile = boshiamy::kProfileGuid;
    profile.category = GUID_TFCAT_TIP_KEYBOARD;
    profile.api_description = profile.registry.description = boshiamy::profile_display::kKnownOldDescription;
    profile.registry.icon_file = old_dll.wstring();
    profile.enabled = TRUE;
    profile.active_result = profile.default_result = S_FALSE;
    display.value.descriptions.registry64 = display.value.descriptions.registry32 =
        profile_display_plan::detail::terminated_reg_sz(profile.api_description);

    const bool late_failure = kind == 3;
    LateFailureBackend late_backend(registry_backend);
    Backend& selected = late_failure ? static_cast<Backend&>(late_backend) : static_cast<Backend&>(registry_backend);
    profile_display_backend::DisplayBackend backend(selected, display,
        profile_display_backend::Mode::existing_profile_metadata);
    ResourceStates before{};
    CHECK(detail::read_all(backend, before) == S_OK);

    fs::path target;
    if (kind == 1) target = sandbox / L"missing parent" / L"managed root";
    else if (kind == 2) target = sandbox / L"ledger parent" / L"managed root";
    else target = sandbox / L"managed root";
    if (kind == 2) {
        const fs::path pending = target.parent_path() / L".liukai-installer-ledger" /
            L"LiuKai-TSF-v1" / L"pending-evidence";
        CHECK(fs::create_directories(pending));
        std::ofstream journal(pending / L"transaction.journal", std::ios::binary);
        CHECK(static_cast<bool>(journal));
        journal << "deliberately unresolved test journal\n";
        journal.close();
        CHECK(static_cast<bool>(journal));
    }

    // The legacy package bridge must reject machine opt-in before inspecting
    // source/package paths, creating a ledger, or changing registrations.
    dev_host_package_migration::Request unsupported;
    unsupported.deployment.include_machine_com_x64=true;
    CHECK(dev_host_package_migration::execute(backend,unsupported).operation==E_INVALIDARG);

    InstallerCommand command = parse_installer_command({L"--migrate-dev-host", package.wstring(), target.wstring(),
        old_dll.wstring(), files::detail::widen_ascii(source_hash)});
    const LifecycleResult result = execute_installer_command(backend, command);
    std::cout << "case=" << kind << " operation=" << static_cast<unsigned long>(result.operation)
              << " cleanup=" << static_cast<unsigned long>(result.cleanup) << '\n';
    ResourceStates after{};
    CHECK(detail::read_all(backend, after) == S_OK);
    CHECK(files::detail::sha256_file(old_dll) == source_hash);
    CHECK(shared.writes == 0 && shared.health == 0 && display.writes == 0);
    for (size_t i = 1; i < before.size(); ++i) CHECK(after[i] == before[i]);

    if (kind == 0) {
        files::DeploymentSnapshot installed;
        CHECK(result.operation == S_OK && result.cleanup == S_OK);
        CHECK(files::FileDeployment::inspect(target, "LiuKai-TSF-v1", &installed) == S_OK);
        CHECK(after[0].path == installed.x64_dll.wstring());
        CHECK(!fs::exists(target / L"versions" / files::detail::widen_ascii(installed.version) / L"x86"));
        CHECK(dev_host_migration_history::inspect(target) == S_OK);
        std::cout << "PASS space-containing target; COM64 migrated; COM32/profile/shared unchanged\n";
        return true;
    }
    if (kind == 1) {
        CHECK(result.operation != S_OK && result.cleanup == S_OK);
        CHECK(after == before && !fs::exists(target) && !fs::exists(target.parent_path()));
        CHECK(!fs::exists(dev_host_migration_history::ledger_base(target)));
        std::cout << "PASS missing immediate parent rejected before mutation\n";
        return true;
    }
    if (kind == 2) {
        CHECK(result.operation != S_OK && result.cleanup == S_OK);
        CHECK(after == before && !fs::exists(target));
        CHECK(dev_host_migration_history::inspect(target) != S_OK);
        std::cout << "PASS unresolved destination-parent journal rejected before mutation\n";
        return true;
    }
    CHECK(kind == 3 && late_backend.injected);
    CHECK(result.operation != S_OK && result.cleanup == S_OK);
    CHECK(after == before && !fs::exists(target));
    CHECK(dev_host_migration_history::inspect(target) != S_OK);
    std::cout << "PASS late post-COM-write failure restored exact state and removed fresh package root\n";
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path package = fs::absolute(argv[1]).lexically_normal();
    const fs::path root = fs::absolute(argv[2]).lexically_normal();
    if (root.wstring().find(L' ') == std::wstring::npos) return 2;
    std::error_code ec;
    if (fs::exists(root, ec) || ec || !fs::create_directory(root, ec) || ec) return 2;
    for (unsigned kind = 0; kind != 4; ++kind)
        if (!run_case(package, root / (L"case " + std::to_wstring(kind)), kind)) return 1;
    std::cout << "PASS all isolated cases; custom GUID HKCU only; fake TSF/profile backends; no live profile or AppContainer acceptance\n";
    return 0;
}
