#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "../installer/command_line.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return false; } } while (0)
namespace fs = std::filesystem;
using namespace boshiamy::installer;

template<class T, class = void> struct has_deployment_request : std::false_type {};
template<class T> struct has_deployment_request<T, std::void_t<decltype(std::declval<T&>().deployment)>> : std::true_type {};

struct PackageCliBackend final : Backend {
    std::array<ResourceState, static_cast<size_t>(Resource::count)> states{};
    std::vector<Resource> applied, restored;
    Resource fail_apply = Resource::count;
    unsigned writes = 0;
    std::function<void(Resource)> before_read;
    std::array<unsigned, static_cast<size_t>(Resource::count)> read_counts{};
    Resource fail_read = Resource::count;
    unsigned fail_read_on_count = 0;
    HRESULT read(Resource resource, ResourceState* out) override {
        if (!out) return E_POINTER;
        if(before_read)before_read(resource);
        const unsigned count=++read_counts[static_cast<size_t>(resource)];
        if(resource==fail_read&&count==fail_read_on_count)return E_ACCESSDENIED;
        *out = states[static_cast<size_t>(resource)];
        return S_OK;
    }
    HRESULT apply(Resource resource, const ResourceState& desired) override {
        applied.push_back(resource);
        ++writes;
        if (resource == fail_apply) return E_ACCESSDENIED;
        states[static_cast<size_t>(resource)] = desired;
        return S_OK;
    }
    HRESULT restore(Resource resource, const ResourceState& snapshot) override {
        restored.push_back(resource);
        states[static_cast<size_t>(resource)] = snapshot;
        return S_OK;
    }
    bool failure_may_have_mutated(Resource resource, HRESULT) const override { return resource != fail_apply; }
    HRESULT validate_install(const InstallRequest& request) override {
        if (!fs::is_regular_file(request.x64_binary_path) ||
            (request.include_x86 && !fs::is_regular_file(request.x86_binary_path)) ||
            !fs::is_regular_file(request.icon_path))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return S_OK;
    }
};

struct RecordingTransactionObserver final : TransactionObserver {
    struct After { Resource resource; TransactionMutation mutation; HRESULT operation; HRESULT readback; bool may_mutate; bool has_observed; };
    std::vector<After> afters;
    HRESULT begin(TransactionOperation, const ResourceStates&) override { return S_OK; }
    HRESULT before(Resource, TransactionMutation, const ResourceState&, const ResourceState&) override { return S_OK; }
    HRESULT after(Resource resource, TransactionMutation mutation, HRESULT operation_status,
                  HRESULT readback_status, const ResourceState* observed, bool may_mutate) override {
        afters.push_back({resource,mutation,operation_status,readback_status,may_mutate,observed!=nullptr});
        return S_OK;
    }
};

static bool test_x64_only_uninstall(const fs::path& sandbox) {
    const std::wstring x64_path=L"C:\\LiuKai\\versions\\v1\\x64\\BoshiamyTsf.dll";
    InstallerCommand parsed{};
    try { parsed=parse_installer_command({L"--uninstall-x64",x64_path}); }
    catch(const std::exception& error) { std::cerr<<"x64-only uninstall command rejected: "<<error.what()<<"\n";return false; }
    CHECK(static_cast<int>(parsed.action)==5);
    CHECK(!parsed.request.include_x86&&parsed.request.expected_x64_path==x64_path&&parsed.request.expected_x86_path.empty());
    bool bad_refused=false;try{(void)parse_installer_command({L"--uninstall-x64",L"relative.dll"});}catch(const std::invalid_argument&){bad_refused=true;}CHECK(bad_refused);
    const auto legacy=parse_installer_command({L"--uninstall",x64_path,L"C:\\LiuKai\\versions\\v1\\x86\\BoshiamyTsf.dll",
        L"C:\\LiuKai\\shared\\branding.dll",x64_path,L"C:\\LiuKai\\versions\\v1\\x86\\BoshiamyTsf.dll"});
    CHECK(static_cast<int>(legacy.action)==3&&legacy.request.include_x86&&
          !legacy.request.expected_x64_path.empty()&&!legacy.request.expected_x86_path.empty());

    const auto owner=parsed.request.owner;
    const ResourceState own_x64{true,true,owner,x64_path,L"Apartment",{}};
    const ResourceState own_profile{true,true,owner,L"C:\\LiuKai\\shared\\branding.dll",L"",{}};
    const ResourceState own_keyboard{true,true,owner,L"",L"",{}};
    const ResourceState own_tray{true,true,owner,L"",L"",{}};
    const Resource shared_resources[]={Resource::profile,Resource::keyboard_category,Resource::system_tray_category};
    const ResourceState owned_shared[]={own_profile,own_keyboard,own_tray};

    const ResourceState x86_variants[]={
        {true,true,owner,L"C:\\old\\owned32.dll",L"Apartment",{}},
        {true,false,L"foreign-owner",L"C:\\foreign\\tip32.dll",L"Apartment",{}},
        {true,true,owner,L"C:\\unexpected\\mismatched32.dll",L"Apartment",{}}
    };
    for(size_t variant=0;variant<std::size(x86_variants);++variant){
        PackageCliBackend backend;
        backend.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
        backend.states[static_cast<size_t>(Resource::com_x86)]=x86_variants[variant];
        for(size_t i=0;i<std::size(shared_resources);++i)backend.states[static_cast<size_t>(shared_resources[i])]=owned_shared[i];
        if(variant==1)backend.states[static_cast<size_t>(Resource::profile)]={true,false,L"foreign-profile",L"C:\\foreign\\branding.dll",L"",{}};
        const auto before=backend.states;
        const auto result=execute_installer_command(backend,parsed);
        CHECK(SUCCEEDED(result.operation)&&SUCCEEDED(result.cleanup));
        CHECK(!backend.states[static_cast<size_t>(Resource::com_x64)].present);
        CHECK(backend.states[static_cast<size_t>(Resource::com_x86)]==before[static_cast<size_t>(Resource::com_x86)]);
        for(Resource resource:shared_resources)CHECK(backend.states[static_cast<size_t>(resource)]==before[static_cast<size_t>(resource)]);
        CHECK(backend.applied==std::vector<Resource>{Resource::com_x64});
    }

    PackageCliBackend legacy_backend;
    legacy_backend.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    legacy_backend.states[static_cast<size_t>(Resource::com_x86)]={true,true,owner,L"C:\\LiuKai\\versions\\v1\\x86\\BoshiamyTsf.dll",L"Apartment",{}};
    for(size_t i=0;i<std::size(shared_resources);++i)legacy_backend.states[static_cast<size_t>(shared_resources[i])]=owned_shared[i];
    const auto legacy_result=execute_installer_command(legacy_backend,legacy);
    CHECK(SUCCEEDED(legacy_result.operation)&&SUCCEEDED(legacy_result.cleanup));
    CHECK(!legacy_backend.states[static_cast<size_t>(Resource::com_x64)].present&&
          !legacy_backend.states[static_cast<size_t>(Resource::com_x86)].present);
    for(Resource resource:shared_resources)CHECK(!legacy_backend.states[static_cast<size_t>(resource)].present);

    // With no COM32 view, remove only shared state whose owner marker proves
    // lifecycle ownership; borrowed profile state is retained.
    PackageCliBackend last_arch;
    last_arch.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    const ResourceState borrowed_profile{true,false,L"legacy-owner",L"C:\\legacy\\branding.dll",L"",{}};
    last_arch.states[static_cast<size_t>(Resource::profile)]=borrowed_profile;
    last_arch.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    last_arch.states[static_cast<size_t>(Resource::system_tray_category)]=own_tray;
    auto last_arch_command=parsed;
    const auto absent32=execute_installer_command(last_arch,last_arch_command);
    CHECK(SUCCEEDED(absent32.operation)&&SUCCEEDED(absent32.cleanup));
    CHECK(!last_arch.states[static_cast<size_t>(Resource::com_x64)].present);
    CHECK(last_arch.states[static_cast<size_t>(Resource::com_x86)]==ResourceState{});
    CHECK(last_arch.states[static_cast<size_t>(Resource::profile)]==borrowed_profile);
    CHECK(!last_arch.states[static_cast<size_t>(Resource::keyboard_category)].present);
    CHECK(!last_arch.states[static_cast<size_t>(Resource::system_tray_category)].present);

    // A COM32 registration appearing after the initial prestate snapshot must
    // block owned shared cleanup and roll the x64 COM removal back.
    PackageCliBackend drift;
    drift.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    drift.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    drift.states[static_cast<size_t>(Resource::system_tray_category)]=own_tray;
    unsigned com32_reads=0;
    const ResourceState late_x86{true,false,L"racing-owner",L"C:\\raced\\tip32.dll",L"Apartment",{}};
    drift.before_read=[&](Resource resource){if(resource==Resource::com_x86&&++com32_reads==3)drift.states[static_cast<size_t>(Resource::com_x86)]=late_x86;};
    const auto drift_result=execute_installer_command(drift,parsed);
    CHECK(drift_result.operation==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&SUCCEEDED(drift_result.cleanup));
    CHECK(drift.states[static_cast<size_t>(Resource::com_x64)]==own_x64);
    CHECK(drift.states[static_cast<size_t>(Resource::com_x86)]==late_x86);
    CHECK(drift.states[static_cast<size_t>(Resource::keyboard_category)]==own_keyboard);
    CHECK(drift.states[static_cast<size_t>(Resource::system_tray_category)]==own_tray);
    CHECK(drift.applied==std::vector<Resource>({Resource::com_x64,Resource::keyboard_category}));
    CHECK(drift.restored==std::vector<Resource>({Resource::keyboard_category,Resource::com_x64}));

    // COM32 appearing at the first shared guard blocks all shared writes and
    // rolls back only the already removed x64 registration.
    PackageCliBackend first_guard_drift;
    first_guard_drift.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    first_guard_drift.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    first_guard_drift.states[static_cast<size_t>(Resource::system_tray_category)]=own_tray;
    first_guard_drift.before_read=[&](Resource resource){
        if(resource==Resource::com_x86&&first_guard_drift.read_counts[static_cast<size_t>(resource)]==1)
            first_guard_drift.states[static_cast<size_t>(Resource::com_x86)]=late_x86;
    };
    const auto first_guard_result=execute_installer_command(first_guard_drift,parsed);
    CHECK(first_guard_result.operation==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&SUCCEEDED(first_guard_result.cleanup));
    CHECK(first_guard_drift.states[static_cast<size_t>(Resource::com_x64)]==own_x64);
    CHECK(first_guard_drift.states[static_cast<size_t>(Resource::com_x86)]==late_x86);
    CHECK(first_guard_drift.states[static_cast<size_t>(Resource::keyboard_category)]==own_keyboard);
    CHECK(first_guard_drift.states[static_cast<size_t>(Resource::system_tray_category)]==own_tray);
    CHECK(first_guard_drift.applied==std::vector<Resource>{Resource::com_x64});
    CHECK(first_guard_drift.restored==std::vector<Resource>{Resource::com_x64});

    // A failed guard read retains its HRESULT and restores x64 without any
    // shared mutation.
    PackageCliBackend guard_read_failure;
    guard_read_failure.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    guard_read_failure.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    guard_read_failure.fail_read=Resource::com_x86;
    guard_read_failure.fail_read_on_count=2; // initial snapshot succeeds; first guard read fails
    const auto guard_read_result=execute_installer_command(guard_read_failure,parsed);
    CHECK(guard_read_result.operation==E_ACCESSDENIED&&SUCCEEDED(guard_read_result.cleanup));
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::com_x64)]==own_x64);
    CHECK(guard_read_failure.states[static_cast<size_t>(Resource::keyboard_category)]==own_keyboard);
    CHECK(guard_read_failure.applied==std::vector<Resource>{Resource::com_x64});
    CHECK(guard_read_failure.restored==std::vector<Resource>{Resource::com_x64});

    // Exercise the durable-observer path directly: the failed guard closes the
    // keyboard intent as an unmutated observation, then records x64 rollback.
    PackageCliBackend observed_guard_failure;
    observed_guard_failure.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    observed_guard_failure.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    observed_guard_failure.states[static_cast<size_t>(Resource::system_tray_category)]=own_tray;
    observed_guard_failure.before_read=[&](Resource resource){
        if(resource==Resource::com_x86&&observed_guard_failure.read_counts[static_cast<size_t>(resource)]==1)
            observed_guard_failure.states[static_cast<size_t>(Resource::com_x86)]=late_x86;
    };
    RecordingTransactionObserver observer;
    const auto observed_result=uninstall(observed_guard_failure,parsed.request,&observer);
    CHECK(observed_result.operation==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&SUCCEEDED(observed_result.cleanup));
    CHECK(observed_guard_failure.states[static_cast<size_t>(Resource::com_x64)]==own_x64);
    CHECK(observed_guard_failure.states[static_cast<size_t>(Resource::com_x86)]==late_x86);
    CHECK(observed_guard_failure.states[static_cast<size_t>(Resource::keyboard_category)]==own_keyboard);
    CHECK(observed_guard_failure.applied==std::vector<Resource>{Resource::com_x64});
    CHECK(observed_guard_failure.restored==std::vector<Resource>{Resource::com_x64});
    bool closed_keyboard_intent=false;
    for(const auto& event:observer.afters)if(event.resource==Resource::keyboard_category&&
        event.mutation==TransactionMutation::apply&&event.operation==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&
        event.readback==S_OK&&!event.may_mutate&&event.has_observed)closed_keyboard_intent=true;
    CHECK(closed_keyboard_intent);

    // A shared deletion failure restores the already removed x64 COM state.
    PackageCliBackend cleanup_failure;
    cleanup_failure.states[static_cast<size_t>(Resource::com_x64)]=own_x64;
    cleanup_failure.states[static_cast<size_t>(Resource::keyboard_category)]=own_keyboard;
    cleanup_failure.fail_apply=Resource::keyboard_category;
    const auto cleanup_result=execute_installer_command(cleanup_failure,parsed);
    CHECK(FAILED(cleanup_result.operation)&&SUCCEEDED(cleanup_result.cleanup));
    CHECK(cleanup_failure.states[static_cast<size_t>(Resource::com_x64)]==own_x64);
    CHECK(cleanup_failure.states[static_cast<size_t>(Resource::com_x86)]==ResourceState{});
    CHECK(cleanup_failure.states[static_cast<size_t>(Resource::keyboard_category)]==own_keyboard);

    PackageCliBackend stale_x64;
    stale_x64.states[static_cast<size_t>(Resource::com_x64)]={true,true,owner,L"C:\\different\\tip.dll",L"Apartment",{}};
    const auto stale=execute_installer_command(stale_x64,parsed);
    CHECK(FAILED(stale.operation)&&stale_x64.applied.empty());
    (void)sandbox;
    return true;
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),{});
}
static void write_text(const fs::path& path,const std::string& text) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(text.data(),static_cast<std::streamsize>(text.size()));
}
static bool make_x64_only_package(const fs::path& source,const fs::path& destination,
                                  const std::string& version,const std::string& target_version) {
    std::error_code ec;fs::copy(source,destination,fs::copy_options::recursive,ec);if(ec)return false;
    auto manifest=read_text(destination/L"manifest.json");
    if(version!=target_version){
        const std::string old_prefix="versions/"+version+"/",new_prefix="versions/"+target_version+"/";
        for(size_t at=0;(at=manifest.find(old_prefix,at))!=std::string::npos;at+=new_prefix.size())manifest.replace(at,old_prefix.size(),new_prefix);
        const std::string identity="\"version\": \""+version+"\"";const auto at=manifest.find(identity);if(at==std::string::npos)return false;
        manifest.replace(at,identity.size(),"\"version\": \""+target_version+"\"");
        fs::rename(destination/L"versions"/fs::path(std::wstring(version.begin(),version.end())),destination/L"versions"/fs::path(std::wstring(target_version.begin(),target_version.end())),ec);if(ec)return false;
    }
    const std::string both="  \"architectures\": [\r\n    \"x64\",\r\n    \"x86\"\r\n  ],";
    const std::string x64="  \"architectures\": [\r\n    \"x64\"\r\n  ],";
    const auto arch=manifest.find(both);if(arch==std::string::npos)return false;manifest.replace(arch,both.size(),x64);
    const std::string first_x86="    {\r\n      \"path\": \"versions/"+target_version+"/x86/";
    const auto row=manifest.find(first_x86);if(row==std::string::npos)return false;
    const auto comma=manifest.rfind(',',row);const auto end=manifest.find("  ],\r\n  \"manifest_sha256_scope\"",row);
    if(comma==std::string::npos||end==std::string::npos)return false;
    manifest.erase(comma,end-comma);write_text(destination/L"manifest.json",manifest);
    fs::remove_all(destination/L"versions"/fs::path(std::wstring(target_version.begin(),target_version.end()))/L"x86",ec);
    return !ec;
}

template<class Command>
static bool execute_package_when_supported(Command& command, PackageCliBackend& backend,
                                           const fs::path& package, const fs::path& root,
                                           bool fail_x86) {
    if constexpr (has_deployment_request<Command>::value) {
        CHECK(static_cast<int>(command.action) == 4);
        command.deployment.package_root = package;
        command.deployment.install_root = root;
        command.deployment.owner = "LiuKai-TSF-v1";
        command.deployment.expected_old_version.clear();
        command.deployment.expected_old_cli_sha256.clear();
        command.deployment.expected_old_branding_sha256.clear();
        if (fail_x86) backend.fail_apply = Resource::com_x86;
        const auto result = execute_installer_command(backend, command);
        if (!fail_x86) {
            CHECK(SUCCEEDED(result.operation) && SUCCEEDED(result.cleanup));
            CHECK(fs::is_regular_file(root / L"LiuKaiInstaller.exe"));
            CHECK(fs::is_regular_file(root / L"shared" / L"branding.dll"));
            CHECK(backend.states[static_cast<size_t>(Resource::com_x64)].present);
            CHECK(backend.states[static_cast<size_t>(Resource::com_x86)].present);
            CHECK(fs::is_regular_file(backend.states[static_cast<size_t>(Resource::com_x64)].path));
            CHECK(fs::is_regular_file(backend.states[static_cast<size_t>(Resource::com_x86)].path));
        } else {
            CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
            CHECK(!fs::exists(root));
            CHECK(backend.states[static_cast<size_t>(Resource::com_x64)] == ResourceState{});
            CHECK(backend.states[static_cast<size_t>(Resource::com_x86)] == ResourceState{});
            CHECK(backend.restored.size() == 1 && backend.restored[0] == Resource::com_x64);
        }
        return true;
    } else {
        std::cerr << "package deployment is not exposed by InstallerCommand yet\n";
        return false;
    }
}
static bool test_package_parser_refuses_ambiguous_tokens(const fs::path& package, const fs::path& sandbox) {
    const auto base = std::vector<std::wstring>{L"--deploy-package", package.wstring(), sandbox.wstring(), L"-", L"-", L"-"};
    auto expect_refused = [](const std::vector<std::wstring>& args) {
        try { (void)parse_installer_command(args); return false; } catch (const std::invalid_argument&) { return true; }
    };
    auto relative = base; relative[1] = L"relative-package"; CHECK(expect_refused(relative));
    auto partial = base; partial[3] = L"1.0.0"; CHECK(expect_refused(partial));
    auto bad_hash = base; bad_hash[3] = L"1.0.0"; bad_hash[4] = L"ABC"; bad_hash[5] = std::wstring(64, L'0'); CHECK(expect_refused(bad_hash));
    auto extra = base; extra.push_back(L"unexpected"); CHECK(expect_refused(extra));
    return true;
}
static bool test_invalid_architecture_lists_refused(const fs::path& package,const fs::path& sandbox) {
    const auto original=read_text(package/L"manifest.json");
    const auto start=original.find("\"architectures\": [");CHECK(start!=std::string::npos);
    const auto open=original.find('[',start),close=original.find(']',open);CHECK(open!=std::string::npos&&close!=std::string::npos);
    const std::vector<std::string> invalid={"[]","[\"x64\",\"x64\"]","[\"x64\",\"arm64\"]","[\"x64\",\"\"]"};
    for(size_t i=0;i<invalid.size();++i){
        const auto copy=sandbox/(L"invalid-architectures-"+std::to_wstring(i));std::error_code ec;
        fs::copy(package,copy,fs::copy_options::recursive,ec);CHECK(!ec);
        auto changed=original;changed.replace(open,close-open+1,invalid[i]);write_text(copy/L"manifest.json",changed);
        files::detail::Package parsed;CHECK(!files::detail::parse_package(copy,&parsed));
    }
    return true;
}
static bool test_package_command(const fs::path& package, const fs::path& sandbox) {
    CHECK(fs::is_directory(package));
    files::detail::Package validated_package;
    if (!files::detail::parse_package(package,&validated_package)) { std::cerr << "parse_package rejected " << package.string() << "\n"; return false; }
    const auto fresh_root = sandbox / L"fresh-install";
    const std::vector<std::wstring> args = {
        L"--deploy-package", package.wstring(), fresh_root.wstring(), L"-", L"-", L"-"
    };
    InstallerCommand parsed{};
    try { parsed = parse_installer_command(args); }
    catch (const std::exception& error) {
        std::cerr << "package command rejected: " << error.what() << "\n";
        return false;
    }
    PackageCliBackend fresh;
    CHECK(execute_package_when_supported(parsed, fresh, package, fresh_root, false));

    const auto failed_root = sandbox / L"failed-install";
    auto failed_args = args;
    failed_args[2] = failed_root.wstring();
    InstallerCommand failed_command{};
    try { failed_command = parse_installer_command(failed_args); }
    catch (const std::exception& error) {
        std::cerr << "package command rejected for rollback case: " << error.what() << "\n";
        return false;
    }
    PackageCliBackend failing;
    CHECK(execute_package_when_supported(failed_command, failing, package, failed_root, true));
    return true;
}
static bool test_package_expectations_fail_closed(const fs::path& package, const fs::path& sandbox) {
    const auto conflict_root = sandbox / L"foreign-com-root";
    const std::vector<std::wstring> fresh_args = {
        L"--deploy-package", package.wstring(), conflict_root.wstring(), L"-", L"-", L"-"
    };
    const auto fresh_command = parse_installer_command(fresh_args);
    PackageCliBackend conflict;
    conflict.states[static_cast<size_t>(Resource::com_x64)] = {
        true, false, L"LiuKai-TSF-v1", L"C:\\old\\untracked\\tip.dll", L"Apartment", {}
    };
    const auto refused = execute_installer_command(conflict, fresh_command);
    CHECK(FAILED(refused.operation) && SUCCEEDED(refused.cleanup));
    CHECK(conflict.writes == 0 && !fs::exists(conflict_root));

    const auto upgrade_root = sandbox / L"mismatched-upgrade-root";
    const auto install_command = parse_installer_command({
        L"--deploy-package", package.wstring(), upgrade_root.wstring(), L"-", L"-", L"-"
    });
    PackageCliBackend installed;
    const auto installed_result = execute_installer_command(installed, install_command);
    if (FAILED(installed_result.operation)||FAILED(installed_result.cleanup)) std::cerr << "legacy package install failed: operation=" << std::hex << static_cast<unsigned long>(installed_result.operation) << " cleanup=" << static_cast<unsigned long>(installed_result.cleanup) << "\n";
    CHECK(SUCCEEDED(installed_result.operation) && SUCCEEDED(installed_result.cleanup));
    files::DeploymentSnapshot snapshot;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(upgrade_root, "LiuKai-TSF-v1", &snapshot)));
    const auto before_writes = installed.writes;
    std::wstring version(snapshot.version.begin(), snapshot.version.end());
    std::wstring cli_hash = std::wstring(snapshot.installer_cli_sha256.begin(), snapshot.installer_cli_sha256.end());
    std::wstring brand_hash = std::wstring(snapshot.branding_sha256.begin(), snapshot.branding_sha256.end());
    cli_hash.assign(cli_hash.size(), L'0');
    const auto stale_command = parse_installer_command({
        L"--deploy-package", package.wstring(), upgrade_root.wstring(), version, cli_hash, brand_hash
    });
    const auto stale = execute_installer_command(installed, stale_command);
    CHECK(stale.operation == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    CHECK(installed.writes == before_writes);
    files::DeploymentSnapshot after;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(upgrade_root, "LiuKai-TSF-v1", &after)));
    CHECK(after.version == snapshot.version && after.installer_cli_sha256 == snapshot.installer_cli_sha256);
    return true;
}
static bool test_x64_only_package_deployment(const fs::path& package,const fs::path& sandbox) {
    files::detail::Package parsed;
    CHECK(files::detail::parse_package(package,&parsed));

    // A fresh x64-only package ignores a pre-existing COM32 registration and
    // leaves its complete observed state untouched.
    const auto fresh_package=parsed.has_x86?sandbox/L"fresh-x64-package":package;
    if(parsed.has_x86)CHECK(make_x64_only_package(package,fresh_package,parsed.version,parsed.version));
    const auto fresh_root=sandbox/L"fresh-x64-root";
    const auto foreign_x86=ResourceState{true,false,L"foreign-owner",L"C:\\foreign\\legacy32.dll",L"Apartment",{}};
    PackageCliBackend fresh;
    fresh.states[static_cast<size_t>(Resource::com_x86)]=foreign_x86;
    const auto fresh_command=parse_installer_command({L"--deploy-package",fresh_package.wstring(),fresh_root.wstring(),L"-",L"-",L"-"});
    const auto fresh_result=execute_installer_command(fresh,fresh_command);
    CHECK(SUCCEEDED(fresh_result.operation)&&SUCCEEDED(fresh_result.cleanup));
    CHECK(fresh.states[static_cast<size_t>(Resource::com_x86)]==foreign_x86);
    CHECK(std::find(fresh.applied.begin(),fresh.applied.end(),Resource::com_x86)==fresh.applied.end());
    files::DeploymentSnapshot fresh_snapshot;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(fresh_root,"LiuKai-TSF-v1",&fresh_snapshot)));
    CHECK(!fresh_snapshot.has_x86&&fresh_snapshot.x86_dll.empty());
    const auto fresh_x64_hash=files::detail::sha256_file(fresh_snapshot.x64_dll);
    const auto fresh_cli_hash=files::detail::sha256_file(fresh_snapshot.installer_cli);
    const auto fresh_brand_hash=files::detail::sha256_file(fresh_snapshot.branding_dll);
    CHECK(!fresh_x64_hash.empty()&&!fresh_cli_hash.empty()&&!fresh_brand_hash.empty());

    if(!parsed.has_x86){
        const auto owned_x64=fresh.states[static_cast<size_t>(Resource::com_x64)];
        const auto repair_command=parse_installer_command({L"--deploy-package",fresh_package.wstring(),fresh_root.wstring(),
            std::wstring(fresh_snapshot.version.begin(),fresh_snapshot.version.end()),
            std::wstring(fresh_snapshot.installer_cli_sha256.begin(),fresh_snapshot.installer_cli_sha256.end()),
            std::wstring(fresh_snapshot.branding_sha256.begin(),fresh_snapshot.branding_sha256.end())});

        // Simulate a missing owned x64 COM registration, then repair from the
        // same x64-only package while retaining the foreign COM32 snapshot.
        fresh.states[static_cast<size_t>(Resource::com_x64)]=ResourceState{};
        const auto before_repair=fresh.applied.size();
        const auto repaired=execute_installer_command(fresh,repair_command);
        CHECK(SUCCEEDED(repaired.operation)&&SUCCEEDED(repaired.cleanup));
        CHECK(fresh.states[static_cast<size_t>(Resource::com_x64)]==owned_x64);
        CHECK(fresh.states[static_cast<size_t>(Resource::com_x86)]==foreign_x86);
        CHECK(std::find(fresh.applied.begin()+static_cast<std::ptrdiff_t>(before_repair),fresh.applied.end(),Resource::com_x64)!=fresh.applied.end());
        CHECK(std::find(fresh.applied.begin()+static_cast<std::ptrdiff_t>(before_repair),fresh.applied.end(),Resource::com_x86)==fresh.applied.end());
        files::DeploymentSnapshot repaired_snapshot;
        CHECK(SUCCEEDED(files::FileDeployment::inspect(fresh_root,"LiuKai-TSF-v1",&repaired_snapshot)));
        CHECK(!repaired_snapshot.has_x86&&repaired_snapshot.x86_dll.empty()&&repaired_snapshot.version==fresh_snapshot.version);
        CHECK(repaired_snapshot.manifest_sha256==fresh_snapshot.manifest_sha256&&
              repaired_snapshot.installer_cli_sha256==fresh_snapshot.installer_cli_sha256&&
              repaired_snapshot.branding_sha256==fresh_snapshot.branding_sha256&&
              files::detail::sha256_file(repaired_snapshot.x64_dll)==fresh_x64_hash&&
              files::detail::sha256_file(repaired_snapshot.installer_cli)==fresh_cli_hash&&
              files::detail::sha256_file(repaired_snapshot.branding_dll)==fresh_brand_hash);
        CHECK(!fs::exists(fresh_root/L"versions"/std::wstring(repaired_snapshot.version.begin(),repaired_snapshot.version.end())/L"x86"));

        const auto before_healthy_retry=fresh.applied.size();
        const auto healthy_retry=execute_installer_command(fresh,repair_command);
        CHECK(SUCCEEDED(healthy_retry.operation)&&SUCCEEDED(healthy_retry.cleanup));
        CHECK(fresh.applied.size()==before_healthy_retry);
        CHECK(fresh.states[static_cast<size_t>(Resource::com_x64)]==owned_x64);
        CHECK(fresh.states[static_cast<size_t>(Resource::com_x86)]==foreign_x86);
        return true;
    }

    // Upgrading a legacy dual-architecture installation keeps its existing
    // x86 registration and immutable old x86 payload while publishing x64 only.
    const auto upgrade_root=sandbox/L"dual-to-x64-root";
    PackageCliBackend upgrade;
    const auto legacy_command=parse_installer_command({L"--deploy-package",package.wstring(),upgrade_root.wstring(),L"-",L"-",L"-"});
    const auto legacy_result=execute_installer_command(upgrade,legacy_command);
    CHECK(SUCCEEDED(legacy_result.operation)&&SUCCEEDED(legacy_result.cleanup));
    files::DeploymentSnapshot before;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(upgrade_root,"LiuKai-TSF-v1",&before))&&before.has_x86);
    const auto old_x86_state=upgrade.states[static_cast<size_t>(Resource::com_x86)];
    const auto old_x86_hash=files::detail::sha256_file(before.x86_dll);
    CHECK(!old_x86_hash.empty());
    const auto next_version=parsed.version+".x64";
    const auto upgrade_package=sandbox/L"upgrade-x64-package";
    CHECK(make_x64_only_package(package,upgrade_package,parsed.version,next_version));
    const auto command=parse_installer_command({L"--deploy-package",upgrade_package.wstring(),upgrade_root.wstring(),
        std::wstring(before.version.begin(),before.version.end()),
        std::wstring(before.installer_cli_sha256.begin(),before.installer_cli_sha256.end()),
        std::wstring(before.branding_sha256.begin(),before.branding_sha256.end())});
    const auto apply_count=upgrade.applied.size();
    const auto result=execute_installer_command(upgrade,command);
    CHECK(SUCCEEDED(result.operation)&&SUCCEEDED(result.cleanup));
    CHECK(upgrade.states[static_cast<size_t>(Resource::com_x86)]==old_x86_state);
    CHECK(std::find(upgrade.applied.begin()+static_cast<std::ptrdiff_t>(apply_count),upgrade.applied.end(),Resource::com_x86)==upgrade.applied.end());
    CHECK(files::detail::sha256_file(before.x86_dll)==old_x86_hash);
    files::DeploymentSnapshot after;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(upgrade_root,"LiuKai-TSF-v1",&after)));
    CHECK(!after.has_x86&&after.x86_dll.empty()&&after.version==next_version);
    CHECK(!fs::exists(upgrade_root/L"versions"/std::wstring(after.version.begin(),after.version.end())/L"x86"));
    return true;
}
static bool test_old_cli_contract_remains() {
    const auto status = parse_installer_command({L"--status"});
    CHECK(static_cast<int>(status.action) == 0);
    const auto old = parse_installer_command({L"--install", L"C:\\LiuKai\\x64\\tip.dll",
        L"C:\\LiuKai\\x86\\tip.dll", L"C:\\LiuKai\\shared\\branding.dll", L"-", L"-"});
    CHECK(static_cast<int>(old.action) == 1);
    CHECK(old.request.x64_binary_path != old.request.x86_binary_path);
    PackageCliBackend backend;
    const auto inspected = execute_installer_command(backend, status);
    CHECK(SUCCEEDED(inspected.operation) && SUCCEEDED(inspected.cleanup) && backend.writes == 0);
    InstallerCommand invalid{}; invalid.action=static_cast<InstallerAction>(99);
    const auto refused=execute_installer_command(backend,invalid);
    CHECK(refused.operation==E_INVALIDARG && backend.writes==0);
    return true;
}
int wmain(int argc, wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc != 3 || !fs::path(argv[1]).is_absolute() || !fs::path(argv[2]).is_absolute()) {
        std::wcerr << L"usage: installer-package-cli-tests.exe <absolute-package-dir> <absolute-new-sandbox>\n";
        return 2;
    }
    const fs::path package = argv[1], sandbox = argv[2];
    files::detail::Package package_info;
    if(!files::detail::parse_package(package,&package_info)) { std::cerr << "input package is invalid\n"; return 1; }
    std::error_code ec;
    if (fs::exists(sandbox, ec) || ec) {
        std::wcerr << L"test sandbox must not exist\n";
        return 2;
    }
    fs::create_directories(sandbox, ec);
    if (ec) return 2;
    if (!test_old_cli_contract_remains() || !test_x64_only_uninstall(sandbox) || !test_x64_only_package_deployment(package,sandbox)) return 1;
    if(!test_invalid_architecture_lists_refused(package,sandbox))return 1;
    if(package_info.has_x86&&(!test_package_parser_refuses_ambiguous_tokens(package, sandbox) ||
        !test_package_expectations_fail_closed(package, sandbox) || !test_package_command(package, sandbox))) return 1;
    std::cout << (package_info.has_x86?"dual-arch and x64-only package lifecycle CLI fake-backend execution passed":"x64-only package lifecycle CLI fake-backend execution passed") << "; no product registry writes\n";
    return 0;
}
