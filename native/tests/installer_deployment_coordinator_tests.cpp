#include <windows.h>
#include <filesystem>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../installer/deployment_coordinator.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return false; } } while (0)
namespace fs = std::filesystem;
using namespace boshiamy::installer;
using namespace boshiamy::installer::files;
namespace file_detail = boshiamy::installer::files::detail;

struct FakeBackend final : Backend {
    std::array<ResourceState, static_cast<size_t>(Resource::count)> state{};
    std::vector<Resource> applied, restored;
    Resource fail_apply = Resource::count;
    Resource fail_restore = Resource::count;
    Resource throw_on_apply = Resource::count;
    bool mutate_before_fail = false;
    bool fail_after_external_branding_change = false;
    bool inject_finalize_conflict = false;
    bool restore_observed_live_files = false;
    fs::path installed_x64, installed_x86, stable_branding;

    HRESULT read(Resource resource, ResourceState* out) override {
        if (!out) return E_POINTER;
        *out = state[static_cast<size_t>(resource)];
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        applied.push_back(resource);
        if (resource == throw_on_apply) {
            state[static_cast<size_t>(resource)] = desired;
            if (resource == Resource::com_x64) installed_x64 = desired.path;
            if (resource == Resource::com_x86) installed_x86 = desired.path;
            if (resource == Resource::profile) stable_branding = desired.path;
            throw std::runtime_error("injected backend exception after mutation");
        }
        const bool failing = resource == fail_apply;
        if (!failing || mutate_before_fail) state[static_cast<size_t>(resource)] = desired;
        if (resource == Resource::com_x64) installed_x64 = desired.path;
        if (resource == Resource::com_x86) installed_x86 = desired.path;
        if (resource == Resource::profile) stable_branding = desired.path;
        if (failing && fail_after_external_branding_change) {
            std::ofstream out(stable_branding, std::ios::binary | std::ios::trunc);
            out << "external-writer";
            out.close();
        }
        if (resource == Resource::system_tray_category && inject_finalize_conflict) {
            const auto root = stable_branding.parent_path().parent_path();
            for (const auto& item : fs::directory_iterator(root)) {
                if (item.path().filename().wstring().rfind(L".txn-", 0) == 0) {
                    std::ofstream out(item.path() / L"external-journal-entry", std::ios::binary);
                    out << "unrecognized";
                    out.close();
                    break;
                }
            }
        }
        return failing ? E_ACCESSDENIED : S_OK;
    }
    HRESULT restore(Resource resource, const ResourceState& snapshot) override {
        restored.push_back(resource);
        if (!installed_x64.empty() && !installed_x86.empty())
            restore_observed_live_files = fs::is_regular_file(installed_x64) && fs::is_regular_file(installed_x86);
        if (resource == fail_restore) return E_ACCESSDENIED;
        state[static_cast<size_t>(resource)] = snapshot;
        return S_OK;
    }
    HRESULT validate_install(const InstallRequest& request) override {
        if (!fs::is_regular_file(request.x64_binary_path) || (request.include_x86 && !fs::is_regular_file(request.x86_binary_path)) ||
            !fs::is_regular_file(request.icon_path)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        installed_x64 = request.x64_binary_path;
        installed_x86 = request.x86_binary_path;
        stable_branding = request.icon_path;
        return S_OK;
    }
    bool failure_may_have_mutated(Resource resource, HRESULT) const override {
        return resource == fail_apply && mutate_before_fail;
    }
};

static std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static void write_text(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}
static bool update_manifest_row(const fs::path& root, const std::string& relative, const fs::path& payload) {
    auto manifest = read_text(root / L"manifest.json");
    const auto row = manifest.find(std::string("\"path\": \"") + relative + "\"");
    if (row == std::string::npos) return false;
    const auto hash_key = manifest.find("\"sha256\": \"", row);
    if (hash_key == std::string::npos) return false;
    const auto hash_start = hash_key + std::string("\"sha256\": \"").size();
    const auto hash_end = manifest.find('"', hash_start);
    if (hash_end == std::string::npos) return false;
    manifest.replace(hash_start, hash_end - hash_start, file_detail::sha256_file(payload));
    const auto bytes_key = manifest.find("\"bytes\": ", hash_start);
    if (bytes_key == std::string::npos) return false;
    const auto bytes_start = bytes_key + std::string("\"bytes\": ").size();
    const auto bytes_end = manifest.find_first_not_of("0123456789", bytes_start);
    if (bytes_end == std::string::npos) return false;
    manifest.replace(bytes_start, bytes_end - bytes_start, std::to_string(fs::file_size(payload)));
    write_text(root / L"manifest.json", manifest);
    return true;
}
static bool make_next_package(const fs::path& source, const fs::path& destination,
                              const std::string& old_version, const std::string& new_version) {
    std::error_code ec;
    fs::copy(source, destination, fs::copy_options::recursive, ec);
    if (ec) return false;
    auto manifest = read_text(destination / L"manifest.json");
    const std::string old_prefix = "versions/" + old_version + "/";
    const std::string new_prefix = "versions/" + new_version + "/";
    for (size_t at = 0; (at = manifest.find(old_prefix, at)) != std::string::npos; at += new_prefix.size())
        manifest.replace(at, old_prefix.size(), new_prefix);
    const std::string identity = std::string("\"version\": \"") + old_version + "\"";
    const auto identity_at = manifest.find(identity);
    if (identity_at == std::string::npos) return false;
    manifest.replace(identity_at, identity.size(), std::string("\"version\": \"") + new_version + "\"");
    write_text(destination / L"manifest.json", manifest);
    fs::rename(destination / L"versions" / file_detail::widen_ascii(old_version),
               destination / L"versions" / file_detail::widen_ascii(new_version), ec);
    if (ec) return false;
    for (const auto& relative : {std::string("LiuKaiInstaller.exe"), std::string("shared/branding.dll")}) {
        const fs::path payload = destination / file_detail::widen_ascii(relative);
        std::vector<uint8_t> bytes;
        if (!file_detail::read_bytes(payload, &bytes) || bytes.size() < 64) return false;
        const uint32_t pe = file_detail::u32le(bytes, 0x3c);
        if (pe + 12 > bytes.size()) return false;
        bytes[pe + 8] ^= 0x5a;
        std::ofstream out(payload, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out || !update_manifest_row(destination, relative, payload)) return false;
    }
    return true;
}
static DeploymentRequest request_for(const fs::path& package, const fs::path& install_root) {
    DeploymentRequest request;
    request.package_root = package;
    request.install_root = install_root;
    request.owner = "LiuKai-TSF-v1";
    return request;
}
static bool no_transaction_dirs(const fs::path& root) {
    if (!fs::exists(root)) return true;
    for (const auto& item : fs::directory_iterator(root))
        if (item.path().filename().wstring().rfind(L".txn-", 0) == 0) return false;
    return true;
}
static bool test_fresh_success(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    auto request = request_for(package, sandbox / L"fresh-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(SUCCEEDED(result.operation) && SUCCEEDED(result.cleanup));
    DeploymentSnapshot installed;
    CHECK(SUCCEEDED(FileDeployment::inspect(request.install_root, request.owner, &installed)));
    CHECK(installed.x64_dll.parent_path().parent_path().parent_path().filename() == L"versions");
    CHECK(installed.x64_dll != installed.x86_dll);
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)].path == installed.x64_dll.wstring());
    CHECK(backend.state[static_cast<size_t>(Resource::com_x86)].path == installed.x86_dll.wstring());
    CHECK(backend.state[static_cast<size_t>(Resource::profile)].path == installed.branding_dll.wstring());
    CHECK(no_transaction_dirs(request.install_root));
    return true;
}
static bool test_second_com_failure_rolls_back_files(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.fail_apply = Resource::com_x86;
    auto request = request_for(package, sandbox / L"failed-fresh-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(!fs::exists(request.install_root));
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)] == ResourceState{});
    CHECK(backend.state[static_cast<size_t>(Resource::com_x86)] == ResourceState{});
    CHECK(backend.restored.size() == 1 && backend.restored[0] == Resource::com_x64);
    return true;
}
static bool test_upgrade_failure_preserves_old_version(const fs::path& package, const fs::path& sandbox) {
    const auto root = sandbox / L"upgrade-root";
    auto first = request_for(package, root);
    FakeBackend backend;
    CHECK(SUCCEEDED(deploy_and_register(backend, first).operation));
    DeploymentSnapshot old;
    CHECK(SUCCEEDED(FileDeployment::inspect(root, first.owner, &old)));
    const auto old_cli = file_detail::sha256_file(old.installer_cli), old_brand = file_detail::sha256_file(old.branding_dll);
    const auto second_package = sandbox / L"package-v2";
    file_detail::Package package_info;
    CHECK(file_detail::parse_package(package, &package_info));
    const std::string new_version = package_info.version + ".next";
    CHECK(make_next_package(package, second_package, package_info.version, new_version));
    auto second = request_for(second_package, root);
    second.expected_old_version = old.version;
    second.expected_old_cli_sha256 = old.installer_cli_sha256;
    second.expected_old_branding_sha256 = old.branding_sha256;
    FakeBackend failing = backend;
    failing.applied.clear(); failing.restored.clear(); failing.fail_apply = Resource::com_x86;
    const auto result = deploy_and_register(failing, second);
    CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
    DeploymentSnapshot after;
    CHECK(SUCCEEDED(FileDeployment::inspect(root, first.owner, &after)));
    CHECK(after.version == old.version && after.x64_dll == old.x64_dll && after.x86_dll == old.x86_dll);
    CHECK(file_detail::sha256_file(after.installer_cli) == old_cli && file_detail::sha256_file(after.branding_dll) == old_brand);
    CHECK(!fs::exists(root / L"versions" / file_detail::widen_ascii(new_version)));
    CHECK(failing.state[static_cast<size_t>(Resource::com_x64)] == backend.state[static_cast<size_t>(Resource::com_x64)]);
    CHECK(failing.state[static_cast<size_t>(Resource::com_x86)] == backend.state[static_cast<size_t>(Resource::com_x86)]);
    CHECK(no_transaction_dirs(root));
    return true;
}
static bool test_profile_failure_restores_tsf_before_files(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.fail_apply = Resource::profile;
    backend.mutate_before_fail = true;
    auto request = request_for(package, sandbox / L"profile-failure-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(backend.restore_observed_live_files);
    CHECK(!fs::exists(request.install_root));
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)] == ResourceState{});
    CHECK(backend.state[static_cast<size_t>(Resource::profile)] == ResourceState{});
    return true;
}
static bool test_incomplete_tsf_rollback_preserves_published_files(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.fail_apply = Resource::com_x86;
    backend.fail_restore = Resource::com_x64;
    auto request = request_for(package, sandbox / L"incomplete-tsf-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && FAILED(result.cleanup));
    CHECK(fs::is_regular_file(backend.installed_x64));
    CHECK(fs::is_regular_file(backend.installed_x86));
    CHECK(fs::exists(request.install_root / L".liukai-install-state"));
    CHECK(!no_transaction_dirs(request.install_root));
    DeploymentSnapshot pending;
    CHECK(SUCCEEDED(FileDeployment::inspect(request.install_root, request.owner, &pending)));
    request.expected_old_version = pending.version;
    request.expected_old_cli_sha256 = pending.installer_cli_sha256;
    request.expected_old_branding_sha256 = pending.branding_sha256;
    const auto retry = deploy_and_register(backend, request);
    CHECK(FAILED(retry.operation));
    CHECK(fs::is_regular_file(backend.installed_x64));
    return true;
}
static bool test_throwing_backend_preserves_files_and_journal(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.throw_on_apply = Resource::com_x64;
    auto request = request_for(package, sandbox / L"throwing-backend-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(result.operation == E_UNEXPECTED && result.cleanup == E_UNEXPECTED);
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)].present);
    CHECK(fs::is_regular_file(backend.state[static_cast<size_t>(Resource::com_x64)].path));
    CHECK(fs::is_regular_file(backend.installed_x86));
    CHECK(!no_transaction_dirs(request.install_root));
    return true;
}
static bool test_external_file_change_blocks_file_rollback(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.fail_apply = Resource::com_x86;
    backend.fail_after_external_branding_change = true;
    auto request = request_for(package, sandbox / L"external-change-root");
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && FAILED(result.cleanup));
    CHECK(read_text(request.install_root / L"shared" / L"branding.dll") == "external-writer");
    CHECK(!no_transaction_dirs(request.install_root));
    return true;
}
static bool test_finalize_conflict_retains_journal_and_blocks_reentry(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    backend.inject_finalize_conflict = true;
    auto request = request_for(package, sandbox / L"finalize-conflict-root");
    const auto first = deploy_and_register(backend, request);
    CHECK(SUCCEEDED(first.operation) && FAILED(first.cleanup));
    DeploymentSnapshot committed;
    CHECK(SUCCEEDED(FileDeployment::inspect(request.install_root, request.owner, &committed)));
    CHECK(fs::is_regular_file(committed.x64_dll) && fs::is_regular_file(committed.x86_dll));
    CHECK(!no_transaction_dirs(request.install_root));
    const auto prior_apply_count = backend.applied.size();
    backend.inject_finalize_conflict = false;
    request.expected_old_version = committed.version;
    request.expected_old_cli_sha256 = committed.installer_cli_sha256;
    request.expected_old_branding_sha256 = committed.branding_sha256;
    const auto retry = deploy_and_register(backend, request);
    CHECK(FAILED(retry.operation));
    CHECK(backend.applied.size() == prior_apply_count);
    CHECK(fs::is_regular_file(committed.x64_dll) && fs::is_regular_file(committed.x86_dll));
    CHECK(!no_transaction_dirs(request.install_root));
    return true;
}
static bool test_file_failure_never_calls_backend(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    auto request = request_for(package, sandbox / L"copy-failure-root");
    DeploymentFaults faults;
    faults.after_copy_chunk = [](const std::string&, size_t) { return false; };
    request.faults = &faults;
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation));
    CHECK(backend.applied.empty() && backend.restored.empty());
    CHECK(!fs::exists(request.install_root));
    return true;
}
static bool test_post_rename_version_failure_without_external_edit_rolls_back(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    auto request = request_for(package, sandbox / L"post-version-rename-clean-root");
    DeploymentFaults faults;
    faults.after_rename = [](const std::string& label, const fs::path&) {
        return label != "version-root";
    };
    request.faults = &faults;
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(backend.applied.empty());
    CHECK(!fs::exists(request.install_root));
    return true;
}
static bool test_post_rename_version_external_edit_is_preserved(const fs::path& package, const fs::path& sandbox) {
    FakeBackend backend;
    auto request = request_for(package, sandbox / L"post-version-rename-external-root");
    fs::path published_version;
    DeploymentFaults faults;
    faults.after_rename = [&](const std::string& label, const fs::path& target) {
        if (label != "version-root") return true;
        published_version = target;
        write_text(target / L"x64" / L"BoshiamyTsf.dll", "external-writer");
        return false;
    };
    request.faults = &faults;
    const auto result = deploy_and_register(backend, request);
    CHECK(FAILED(result.operation) && FAILED(result.cleanup));
    CHECK(backend.applied.empty());
    CHECK(read_text(published_version / L"x64" / L"BoshiamyTsf.dll") == "external-writer");
    CHECK(fs::is_directory(published_version));
    return true;
}

// Build a known x64-only package fixture without modifying the input package.
static bool make_machine_package(const fs::path& source,const fs::path& destination) {
    file_detail::Package package;
    CHECK(file_detail::parse_package(source,&package));
    auto manifest=read_text(source/L"manifest.json");
    for(const auto& item:package.files) {
        if(item.first.find("/x86/")!=std::string::npos) {
            const auto at=manifest.find(std::string("\"path\": \"")+item.first+"\"");
            CHECK(at!=std::string::npos);
            size_t begin=manifest.rfind('{',at),end=manifest.find('}',at)+1;
            CHECK(begin!=std::string::npos && end!=0);
            size_t after=end;while(after<manifest.size()&&std::isspace(static_cast<unsigned char>(manifest[after])))++after;
            if(after<manifest.size()&&manifest[after]==',')end=after+1;
            else {while(begin>0&&std::isspace(static_cast<unsigned char>(manifest[begin-1])))--begin;CHECK(begin>0&&manifest[begin-1]==',');--begin;}
            manifest.erase(begin,end-begin);
            continue;
        }
        const auto relative=file_detail::widen_ascii(item.first);
        fs::create_directories((destination/relative).parent_path());
        fs::copy_file(source/relative,destination/relative);
    }
    const auto architectures=manifest.find("\"architectures\"");CHECK(architectures!=std::string::npos);
    const auto begin=manifest.find('[',architectures),end=manifest.find(']',begin);
    CHECK(begin!=std::string::npos&&end!=std::string::npos);
    manifest.replace(begin,end-begin+1,"[\"x64\"]");
    write_text(destination/L"manifest.json",manifest);
    file_detail::Package x64;CHECK(file_detail::parse_package(destination,&x64));
    CHECK(!x64.has_x86);
    return true;
}
static bool test_machine_policy_package(const fs::path& package,const fs::path& sandbox) {
    const auto machine_package=sandbox/L"machine-package";
    CHECK(make_machine_package(package,machine_package));
    auto request=request_for(machine_package,sandbox/L"machine-root");
    request.include_machine_com_x64=true;
    FakeBackend backend;
    bool observed_policy=false;
    const RegistrationExecutor execute=[&](Backend& target,const InstallRequest& registration,TransactionObserver* observer) {
        observed_policy=registration.include_machine_com_x64&&!registration.include_x86;
        return install_or_repair(target,registration,observer);
    };
    CHECK(deploy_and_register(backend,request,nullptr,{},execute).operation==S_OK);
    CHECK(observed_policy);
    DeploymentSnapshot installed;CHECK(FileDeployment::inspect(request.install_root,request.owner,&installed)==S_OK);
    CHECK(backend.state[static_cast<size_t>(Resource::com_x64)].path==installed.x64_dll.wstring());
    CHECK(backend.state[static_cast<size_t>(Resource::machine_com_x64)].path==installed.x64_dll.wstring());
    CHECK(!backend.state[static_cast<size_t>(Resource::com_x86)].present);
    CHECK(no_transaction_dirs(request.install_root));

    // Both registrations and published files compensate after a partial HKLM write.
    auto failure=request_for(machine_package,sandbox/L"machine-failure-root");failure.include_machine_com_x64=true;
    FakeBackend failing;failing.fail_apply=Resource::machine_com_x64;failing.mutate_before_fail=true;
    const auto failed=deploy_and_register(failing,failure);
    CHECK(failed.operation==E_ACCESSDENIED&&failed.cleanup==S_OK);
    CHECK(!fs::exists(failure.install_root));
    CHECK(failing.state[static_cast<size_t>(Resource::com_x64)]==ResourceState{});
    CHECK(failing.state[static_cast<size_t>(Resource::machine_com_x64)]==ResourceState{});

    // Foreign machine state is rejected before file publication or journal creation.
    auto conflict=request_for(machine_package,sandbox/L"machine-conflict-root");conflict.include_machine_com_x64=true;
    FakeBackend foreign;
    foreign.state[static_cast<size_t>(Resource::machine_com_x64)]={true,true,L"Other",L"C:\\foreign\\tsf.dll",L"Apartment",{7,8}};
    const auto foreign_before=foreign.state;
    CHECK(deploy_and_register(foreign,conflict).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(!fs::exists(conflict.install_root));
    CHECK(foreign.applied.empty()&&foreign.restored.empty()&&foreign.state==foreign_before);
    foreign.state={};
    foreign.state[static_cast<size_t>(Resource::profile)]={true,false,L"Other",L"",L"",{4}};
    CHECK(deploy_and_register(foreign,conflict).operation==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
    CHECK(!fs::exists(conflict.install_root)&&foreign.applied.empty());
    return true;
}
static int run(const fs::path& package, const fs::path& sandbox) {
    std::error_code ec;
    if (!fs::is_directory(package) || fs::exists(sandbox)) return 2;
    fs::create_directories(sandbox, ec);
    if (ec) return 2;
    if (!test_machine_policy_package(package, sandbox) ||
        !test_fresh_success(package, sandbox) ||
        !test_second_com_failure_rolls_back_files(package, sandbox) ||
        !test_upgrade_failure_preserves_old_version(package, sandbox) ||
        !test_profile_failure_restores_tsf_before_files(package, sandbox) ||
        !test_incomplete_tsf_rollback_preserves_published_files(package, sandbox) ||
        !test_throwing_backend_preserves_files_and_journal(package, sandbox) ||
        !test_external_file_change_blocks_file_rollback(package, sandbox) ||
        !test_finalize_conflict_retains_journal_and_blocks_reentry(package, sandbox) ||
        !test_file_failure_never_calls_backend(package, sandbox) ||
        !test_post_rename_version_failure_without_external_edit_rolls_back(package, sandbox) ||
        !test_post_rename_version_external_edit_is_preserved(package, sandbox)) return 1;
    std::cout << "deployment coordinator tests passed; fake backend only; product registry untouched\n";
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !fs::path(argv[1]).is_absolute() || !fs::path(argv[2]).is_absolute()) {
        std::wcerr << L"usage: installer-deployment-coordinator-tests.exe <absolute-package-dir> <absolute-new-sandbox>\n";
        return 2;
    }
    return run(argv[1], argv[2]);
}
