#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "../installer/command_line.hpp"
#include "../installer/durable_transaction_observer.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return false; } } while (0)
namespace fs = std::filesystem;
namespace installer = boshiamy::installer;
namespace files = boshiamy::installer::files;
namespace durable = boshiamy::installer::durable_journal;
namespace snapshot = boshiamy::installer::durable_snapshot;
namespace durable_cli = boshiamy::installer::durable_cli;

struct DurableCliBackend final : installer::Backend {
    installer::ResourceStates states{};
    std::vector<installer::Resource> applied;
    std::vector<installer::Resource> restored;
    std::function<void(installer::Resource)> on_read;
    installer::Resource fail_apply_resource=installer::Resource::count;
    bool fail_after_mutating=false;

    HRESULT read(installer::Resource resource, installer::ResourceState* out) override {
        if (!out || resource == installer::Resource::count) return E_POINTER;
        if (on_read) on_read(resource);
        *out = states[static_cast<size_t>(resource)];
        return S_OK;
    }
    HRESULT apply(installer::Resource resource, const installer::ResourceState& desired) override {
        applied.push_back(resource);
        if(resource==fail_apply_resource) {
            if(fail_after_mutating)states[static_cast<size_t>(resource)]=desired;
            return E_ACCESSDENIED;
        }
        states[static_cast<size_t>(resource)] = desired;
        return S_OK;
    }
    HRESULT restore(installer::Resource resource, const installer::ResourceState& before) override {
        restored.push_back(resource);
        states[static_cast<size_t>(resource)] = before;
        return S_OK;
    }
    HRESULT validate_install(const installer::InstallRequest& request) override {
        return fs::is_regular_file(request.x64_binary_path) &&
               fs::is_regular_file(request.x86_binary_path) &&
               fs::is_regular_file(request.icon_path)
            ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
};

static bool make_x64_only_package(const fs::path& package,const fs::path& output,
                                  const std::string& version) {
    std::error_code ec;fs::copy(package,output,fs::copy_options::recursive,ec);if(ec)return false;
    const auto manifest_path=output/L"manifest.json";
    std::ifstream input(manifest_path,std::ios::binary);
    std::string manifest(std::istreambuf_iterator<char>(input),{});
    const std::string both="  \"architectures\": [\r\n    \"x64\",\r\n    \"x86\"\r\n  ],";
    const std::string x64="  \"architectures\": [\r\n    \"x64\"\r\n  ],";
    const auto architecture=manifest.find(both);if(architecture==std::string::npos)return false;
    manifest.replace(architecture,both.size(),x64);
    const std::string first_x86="    {\r\n      \"path\": \"versions/"+version+"/x86/";
    const auto row=manifest.find(first_x86);if(row==std::string::npos)return false;
    const auto previous_comma=manifest.rfind(',',row);
    const auto end=manifest.find("  ],\r\n  \"manifest_sha256_scope\"",row);
    if(previous_comma==std::string::npos||end==std::string::npos)return false;
    manifest.erase(previous_comma,end-previous_comma);
    std::ofstream output_manifest(manifest_path,std::ios::binary|std::ios::trunc);
    output_manifest.write(manifest.data(),static_cast<std::streamsize>(manifest.size()));
    output_manifest.close();if(!output_manifest)return false;
    fs::remove_all(output/L"versions"/files::detail::widen_ascii(version)/L"x86",ec);
    return !ec;
}

static bool test_session_rejects_package_path_architecture_mismatch(
        const fs::path& dual_package,const fs::path& sandbox) {
    files::detail::Package dual_info;
    CHECK(files::detail::parse_package(dual_package,&dual_info)&&dual_info.has_x86);
    const auto x64_package=sandbox/L"x64-only-package";
    CHECK(make_x64_only_package(dual_package,x64_package,dual_info.version));
    files::detail::Package x64_info;
    CHECK(files::detail::parse_package(x64_package,&x64_info)&&!x64_info.has_x86);

    auto expect_rejected_without_ledger=[&](const fs::path& package,bool paths_have_x86,const std::wstring& label)->bool {
        files::detail::Package package_info;CHECK(files::detail::parse_package(package,&package_info));
        files::DeploymentRequest request;
        request.package_root=package;
        const auto case_root=sandbox/label;
        fs::create_directories(case_root);
        request.install_root=case_root/L"install";
        request.owner="LiuKai-TSF-v1";
        files::DeploymentPaths paths;
        paths.version=package_info.version;paths.manifest_sha256=package_info.manifest_sha256;
        paths.installer_cli_sha256=package_info.cli_hash;paths.branding_sha256=package_info.branding_hash;
        paths.version_root=request.install_root/L"versions"/files::detail::widen_ascii(package_info.version);
        paths.x64_dll=paths.version_root/L"x64"/L"BoshiamyTsf.dll";
        paths.has_x86=paths_have_x86;
        if(paths_have_x86)paths.x86_dll=paths.version_root/L"x86"/L"BoshiamyTsf.dll";
        paths.branding_dll=request.install_root/L"shared"/L"branding.dll";
        paths.installer_cli=request.install_root/L"LiuKaiInstaller.exe";
        std::unique_ptr<installer::TransactionObserver> observer;
        const HRESULT result=durable_cli::Session::create(request,paths,&observer);
        CHECK(result==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        CHECK(!observer);
        CHECK(!fs::exists(case_root/L".liukai-installer-ledger"));
        return true;
    };
    CHECK(expect_rejected_without_ledger(dual_package,false,L"dual-with-x64-paths"));
    CHECK(expect_rejected_without_ledger(x64_package,true,L"x64-with-dual-paths"));
    return true;
}

static bool test_status_does_not_create_ledger(const fs::path& sandbox) {
    DurableCliBackend backend;
    const auto command = installer::parse_installer_command({L"--status"});
    const auto result = installer::execute_installer_command(backend, command);
    CHECK(SUCCEEDED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(backend.applied.empty() && backend.restored.empty());
    CHECK(!fs::exists(sandbox / L".liukai-installer-ledger"));
    return true;
}

static bool test_file_phase_fingerprint_v1_compatibility() {
    durable::Identity identity;
    identity.owner="fixture-owner";identity.token="fixture-token";
    identity.package_version="fixture-version";identity.package_manifest_sha256=std::string(64,'a');
    identity.install_root=L"C:\\fixture\\install";
    identity.storage_root=L"C:\\fixture\\ledger\\fixture-token";
    files::DeploymentSnapshot dual;
    dual.version="fixture-version";dual.manifest_sha256=std::string(64,'a');
    dual.installer_cli_sha256=std::string(64,'b');dual.branding_sha256=std::string(64,'c');
    dual.x64_dll=L"C:\\fixture\\install\\versions\\fixture-version\\x64\\BoshiamyTsf.dll";
    dual.x86_dll=L"C:\\fixture\\install\\versions\\fixture-version\\x86\\BoshiamyTsf.dll";
    dual.branding_dll=L"C:\\fixture\\install\\shared\\branding.dll";
    dual.installer_cli=L"C:\\fixture\\install\\LiuKaiInstaller.exe";
    dual.has_x86=true;
    std::string dual_fingerprint;
    CHECK(durable_cli::detail::file_fingerprint(identity,true,&dual,nullptr,&dual_fingerprint));
    CHECK(dual_fingerprint=="f611e20610bc6c07cb889aedb57964ed04e4c095fe53a04cda1b22d71f0b0343");

    auto x64=dual;x64.has_x86=false;x64.x86_dll.clear();
    std::string x64_fingerprint;
    CHECK(durable_cli::detail::file_fingerprint(identity,true,&x64,nullptr,&x64_fingerprint));
    CHECK(x64_fingerprint.size()==64);
    auto inconsistent=dual;inconsistent.x86_dll.clear();
    CHECK(!durable_cli::detail::file_fingerprint(identity,true,&inconsistent,nullptr,&x64_fingerprint));
    inconsistent=x64;inconsistent.x86_dll=L"C:\\fixture\\unexpected32.dll";
    CHECK(!durable_cli::detail::file_fingerprint(identity,true,&inconsistent,nullptr,&x64_fingerprint));
    return true;
}

static bool test_package_command_creates_one_committed_external_session(
        const fs::path& package_root, const fs::path& sandbox) {
    files::detail::Package parsed_package;
    CHECK(files::detail::parse_package(package_root, &parsed_package));
    const std::string manifest_hash = files::detail::sha256_file(package_root / L"manifest.json");
    CHECK(files::detail::valid_hash(manifest_hash));

    const fs::path install_root = sandbox / L"installed-payload";
    const auto command = installer::parse_installer_command({
        L"--deploy-package", package_root.wstring(), install_root.wstring(), L"-", L"-", L"-"});
    DurableCliBackend backend;
    const auto result = installer::execute_installer_command(backend, command);
    CHECK(SUCCEEDED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(backend.applied.size() == snapshot::legacy_resource_count);

    const fs::path owner_root = sandbox / L".liukai-installer-ledger" / L"LiuKai-TSF-v1";
    CHECK(fs::is_directory(owner_root));
    size_t session_count = 0;
    fs::path session_root;
    for (const auto& item : fs::directory_iterator(owner_root)) {
        CHECK(item.is_directory());
        ++session_count;
        session_root = item.path();
    }
    CHECK(session_count == 1);
    const std::string token = session_root.filename().string();
    CHECK(durable::detail::safe_token(token));

    fs::path journal_path, snapshot_path;
    size_t journal_count = 0, snapshot_count = 0;
    for (const auto& item : fs::directory_iterator(session_root)) {
        CHECK(item.is_regular_file());
        const auto extension = item.path().extension().string();
        if (extension == ".journal") { journal_path = item.path(); ++journal_count; }
        if (extension == ".snapshot") { snapshot_path = item.path(); ++snapshot_count; }
    }
    CHECK(journal_count == 1 && snapshot_count == 1);

    durable::Identity identity;
    identity.schema_version = 2;
    identity.owner = "LiuKai-TSF-v1";
    identity.token = token;
    identity.install_root = install_root.wstring();
    identity.storage_root = session_root.wstring();
    identity.package_version = parsed_package.version;
    identity.package_manifest_sha256 = manifest_hash;
    CHECK(durable::detail::valid_identity(identity));

    durable::Store journal(journal_path, identity, session_root);
    durable::Document document;
    CHECK(SUCCEEDED(journal.read(identity, &document)));
    CHECK(document.identity.schema_version == 2);
    CHECK(document.identity.owner == identity.owner && document.identity.token == token);
    CHECK(durable::detail::same_path(document.identity.install_root, install_root.wstring()));
    CHECK(durable::detail::same_path(document.identity.storage_root, session_root.wstring()));
    CHECK(document.identity.package_version == parsed_package.version);
    CHECK(document.identity.package_manifest_sha256 == manifest_hash);
    CHECK(document.committed && document.all_operations_succeeded);

    snapshot::Store saved_snapshot(snapshot_path, identity, session_root);
    installer::ResourceStates initial{};
    std::string snapshot_fingerprint;
    CHECK(SUCCEEDED(saved_snapshot.read(identity, &initial, &snapshot_fingerprint)));
    CHECK(!snapshot_fingerprint.empty());
    for (const auto& state : initial) CHECK(state == installer::ResourceState{});
    return true;
}

static bool test_factory_commit_precedes_file_finalize(
        const fs::path& package_root, const fs::path& sandbox) {
    const fs::path install_root = sandbox / L"commit-before-finalize-install";
    auto command = installer::parse_installer_command({
        L"--deploy-package", package_root.wstring(), install_root.wstring(), L"-", L"-", L"-"});
    DurableCliBackend backend;
    bool injected_finalize_refusal = false;
    backend.on_read = [&](installer::Resource resource) {
        if (injected_finalize_refusal || resource != installer::Resource::system_tray_category ||
            backend.applied.size() != snapshot::legacy_resource_count) return;
        for (const auto& item : fs::directory_iterator(install_root)) {
            const auto name = item.path().filename().wstring();
            if (name.rfind(L".txn-", 0) == 0 && item.is_directory()) {
                std::ofstream foreign(item.path() / L"foreign-finalize-probe", std::ios::binary);
                if (foreign) { foreign << "keep"; foreign.close(); injected_finalize_refusal = static_cast<bool>(foreign); }
                break;
            }
        }
    };
    const auto result = installer::execute_installer_command(backend, command);
    CHECK(injected_finalize_refusal);
    CHECK(SUCCEEDED(result.operation) && FAILED(result.cleanup));
    CHECK(backend.applied.size() == snapshot::legacy_resource_count);

    const fs::path owner_root = sandbox / L".liukai-installer-ledger" / L"LiuKai-TSF-v1";
    CHECK(fs::is_directory(owner_root));
    fs::path session_root;
    size_t sessions = 0;
    for (const auto& item : fs::directory_iterator(owner_root)) {
        CHECK(item.is_directory());
        session_root = item.path();
        ++sessions;
    }
    CHECK(sessions == 1);
    durable::Identity identity;
    files::detail::Package package;
    CHECK(files::detail::parse_package(package_root, &package));
    identity.schema_version = 2;
    identity.owner = "LiuKai-TSF-v1";
    identity.token = session_root.filename().string();
    identity.install_root = install_root.wstring();
    identity.storage_root = session_root.wstring();
    identity.package_version = package.version;
    identity.package_manifest_sha256 = package.manifest_sha256;
    durable::Store journal(session_root / L"transaction.journal", identity, session_root);
    durable::Document document;
    CHECK(SUCCEEDED(journal.read(identity, &document)));
    CHECK(document.committed && document.all_operations_succeeded);
    bool retained_txn = false, retained_probe = false;
    for (const auto& item : fs::directory_iterator(install_root)) {
        if (item.path().filename().wstring().rfind(L".txn-", 0) == 0) {
            retained_txn = true;
            retained_probe = fs::is_regular_file(item.path() / L"foreign-finalize-probe");
        }
    }
    CHECK(retained_txn && retained_probe);
    return true;
}

static bool test_identical_package_retry_is_idempotent(
        const fs::path& package_root, const fs::path& sandbox) {
    const fs::path install_root = sandbox / L"idempotent-package-install";
    DurableCliBackend backend;
    auto first = installer::parse_installer_command({
        L"--deploy-package", package_root.wstring(), install_root.wstring(), L"-", L"-", L"-"});
    const auto installed = installer::execute_installer_command(backend, first);
    CHECK(SUCCEEDED(installed.operation) && SUCCEEDED(installed.cleanup));

    files::DeploymentSnapshot before;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(install_root, "LiuKai-TSF-v1", &before)));
    const size_t writes_before_retry = backend.applied.size();
    const std::wstring version(before.version.begin(), before.version.end());
    const std::wstring cli_hash(before.installer_cli_sha256.begin(), before.installer_cli_sha256.end());
    const std::wstring branding_hash(before.branding_sha256.begin(), before.branding_sha256.end());
    auto retry = installer::parse_installer_command({
        L"--deploy-package", package_root.wstring(), install_root.wstring(),
        version, cli_hash, branding_hash});
    const auto retried = installer::execute_installer_command(backend, retry);
    CHECK(SUCCEEDED(retried.operation) && SUCCEEDED(retried.cleanup));
    CHECK(backend.applied.size() == writes_before_retry && backend.restored.empty());

    files::DeploymentSnapshot after;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(install_root, "LiuKai-TSF-v1", &after)));
    CHECK(after.version == before.version && after.installer_cli_sha256 == before.installer_cli_sha256 &&
          after.branding_sha256 == before.branding_sha256);

    const fs::path owner_root = sandbox / L".liukai-installer-ledger" / L"LiuKai-TSF-v1";
    CHECK(fs::is_directory(owner_root));
    size_t session_count = 0;
    for (const auto& item : fs::directory_iterator(owner_root)) {
        CHECK(item.is_directory());
        const auto session_root = item.path();
        const auto journal_path = session_root / L"transaction.journal";
        CHECK(fs::is_regular_file(journal_path));
        durable::Identity identity;
        files::detail::Package package;
        CHECK(files::detail::parse_package(package_root, &package));
        identity.schema_version = 2;
        identity.owner = "LiuKai-TSF-v1";
        identity.token = session_root.filename().string();
        identity.install_root = install_root.wstring();
        identity.storage_root = session_root.wstring();
        identity.package_version = package.version;
        identity.package_manifest_sha256 = package.manifest_sha256;
        durable::Store journal(journal_path, identity, session_root);
        durable::Document document;
        CHECK(SUCCEEDED(journal.read(identity, &document)));
        CHECK(document.committed && document.all_operations_succeeded);
        ++session_count;
    }
    CHECK(session_count == 2);

    // A different manifest image with the same version and payload rows is
    // not the same package identity. It must not republish or mutate TSF.
    const fs::path altered_package = sandbox / L"altered-package";
    std::error_code ec;
    fs::copy(package_root, altered_package, fs::copy_options::recursive, ec);
    CHECK(!ec && fs::is_directory(altered_package));
    {
        std::ofstream manifest(altered_package / L"manifest.json", std::ios::binary | std::ios::app);
        CHECK(static_cast<bool>(manifest));
        manifest << " \n";
        manifest.close();
        CHECK(static_cast<bool>(manifest));
    }
    auto mismatched_package = installer::parse_installer_command({
        L"--deploy-package", altered_package.wstring(), install_root.wstring(),
        version, cli_hash, branding_hash});
    const auto mismatch = installer::execute_installer_command(backend, mismatched_package);
    CHECK(FAILED(mismatch.operation) && SUCCEEDED(mismatch.cleanup));
    CHECK(backend.applied.size() == writes_before_retry && backend.restored.empty());
    files::DeploymentSnapshot after_mismatch;
    CHECK(SUCCEEDED(files::FileDeployment::inspect(install_root, "LiuKai-TSF-v1", &after_mismatch)));
    CHECK(after_mismatch.version == before.version &&
          after_mismatch.installer_cli_sha256 == before.installer_cli_sha256 &&
          after_mismatch.branding_sha256 == before.branding_sha256);
    return true;
}

static bool test_manifest_drift_after_prepare_refuses_package_deploy(
        const fs::path& package_root, const fs::path& sandbox) {
    const fs::path isolated_package = sandbox / L"mutable-package-fixture";
    std::error_code ec;
    fs::copy(package_root, isolated_package, fs::copy_options::recursive, ec);
    CHECK(!ec && fs::is_directory(isolated_package));
    files::detail::Package prepared_package;
    CHECK(files::detail::parse_package(isolated_package, &prepared_package));
    const std::string prepared_manifest_hash = prepared_package.manifest_sha256;
    CHECK(files::detail::valid_hash(prepared_manifest_hash));

    const fs::path install_root = sandbox / L"manifest-drift-install";
    auto command = installer::parse_installer_command({
        L"--deploy-package", isolated_package.wstring(), install_root.wstring(), L"-", L"-", L"-"});
    bool changed_manifest = false;
    files::DeploymentFaults faults;
    faults.after_copy_verified = [&](const std::string& relative, const fs::path&) {
        if (!changed_manifest && relative.rfind("versions/", 0) == 0) {
            std::ofstream manifest(isolated_package / L"manifest.json", std::ios::binary | std::ios::app);
            if (!manifest) return false;
            manifest << " \n";
            manifest.close();
            changed_manifest = static_cast<bool>(manifest);
        }
        return true;
    };
    command.deployment.faults = &faults;
    DurableCliBackend backend;
    const auto result = installer::execute_installer_command(backend, command);
    CHECK(changed_manifest);
    CHECK(FAILED(result.operation) && SUCCEEDED(result.cleanup));
    CHECK(backend.applied.empty() && backend.restored.empty());
    CHECK(!fs::exists(install_root));

    const fs::path owner_root = sandbox / L".liukai-installer-ledger" / L"LiuKai-TSF-v1";
    CHECK(fs::is_directory(owner_root));
    fs::path session_root;
    size_t sessions = 0;
    for (const auto& item : fs::directory_iterator(owner_root)) {
        CHECK(item.is_directory());
        session_root = item.path();
        ++sessions;
    }
    CHECK(sessions == 1);
    const std::string token = session_root.filename().string();
    CHECK(durable::detail::safe_token(token));

    const fs::path journal_path = session_root / L"transaction.journal";
    const fs::path snapshot_path = session_root / L"transaction.snapshot";
    CHECK(fs::is_regular_file(journal_path) && fs::is_regular_file(snapshot_path));
    durable::Identity identity;
    identity.schema_version = 2;
    identity.owner = "LiuKai-TSF-v1";
    identity.token = token;
    identity.install_root = install_root.wstring();
    identity.storage_root = session_root.wstring();
    identity.package_version = prepared_package.version;
    identity.package_manifest_sha256 = prepared_manifest_hash;
    CHECK(durable::detail::valid_identity(identity));

    durable::Store journal(journal_path, identity, session_root);
    durable::Document document;
    CHECK(SUCCEEDED(journal.read(identity, &document)));
    CHECK(document.identity.schema_version == 2 && document.identity.owner == identity.owner &&
          document.identity.token == token);
    CHECK(durable::detail::same_path(document.identity.install_root, install_root.wstring()));
    CHECK(durable::detail::same_path(document.identity.storage_root, session_root.wstring()));
    CHECK(document.identity.package_version == prepared_package.version &&
          document.identity.package_manifest_sha256 == prepared_manifest_hash);
    CHECK(!document.committed && !document.all_operations_succeeded && document.events.size() == 4);
    CHECK(document.events[0].kind == durable::Event::Kind::intent &&
          document.events[0].intent.resource == "prestate" &&
          document.events[0].intent.action == "capture-install-or-repair");
    CHECK(document.events[1].kind == durable::Event::Kind::observation &&
          document.events[1].observation.operation_status == S_OK &&
          document.events[1].observation.observed_fingerprint ==
              document.events[0].intent.desired_fingerprint);
    CHECK(document.events[2].kind == durable::Event::Kind::intent &&
          document.events[2].intent.resource == "file-publication" &&
          document.events[2].intent.action == "apply");
    CHECK(document.events[3].kind == durable::Event::Kind::observation &&
          FAILED(document.events[3].observation.operation_status));

    snapshot::Store saved_snapshot(snapshot_path, identity, session_root);
    installer::ResourceStates saved_prestate{};
    std::string snapshot_fingerprint;
    CHECK(SUCCEEDED(saved_snapshot.read(identity, &saved_prestate, &snapshot_fingerprint)));
    CHECK(!snapshot_fingerprint.empty() &&
          snapshot_fingerprint == document.events[0].intent.desired_fingerprint);
    for (const auto& state : saved_prestate) CHECK(state == installer::ResourceState{});
    return true;
}


static bool test_machine_session_namespace_snapshot_and_legacy_history(
        const fs::path& dual_package,const fs::path& sandbox) {
    files::detail::Package dual_info;CHECK(files::detail::parse_package(dual_package,&dual_info));
    const auto package=sandbox/L"machine-x64-package";
    CHECK(make_x64_only_package(dual_package,package,dual_info.version));
    files::detail::Package info;CHECK(files::detail::parse_package(package,&info)&&!info.has_x86);
    files::DeploymentRequest request;
    request.package_root=package;request.install_root=sandbox/L"machine-install";
    request.owner="LiuKai-TSF-v1";request.include_machine_com_x64=true;
    files::DeploymentPaths paths;
    paths.version=info.version;paths.manifest_sha256=info.manifest_sha256;
    paths.installer_cli_sha256=info.cli_hash;paths.branding_sha256=info.branding_hash;
    paths.version_root=request.install_root/L"versions"/files::detail::widen_ascii(info.version);
    paths.x64_dll=paths.version_root/L"x64"/L"BoshiamyTsf.dll";
    paths.has_x86=false;paths.branding_dll=request.install_root/L"shared"/L"branding.dll";
    paths.installer_cli=request.install_root/L"LiuKaiInstaller.exe";
    // An immutable old namespace is retained byte for byte and never adopted.
    const auto old_dir=sandbox/L".liukai-installer-ledger"/L"LiuKai-TSF-v1"/L"frozen-old";
    fs::create_directories(old_dir);
    const auto old_path=old_dir/L"transaction.journal";
    {std::ofstream old(old_path,std::ios::binary);old<<"old-scope history must remain immutable";}
    std::unique_ptr<installer::TransactionObserver> observer;
    CHECK(durable_cli::Session::create(request,paths,&observer)==S_OK&&observer);
    auto* session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    const auto identity=session->identity();
    CHECK(identity.schema_version==3&&durable::detail::valid_identity(identity));
    const auto expected=sandbox/durable_cli::ledger_namespace(true)/L"LiuKai-TSF-v1"/
        files::detail::widen_ascii(identity.token);
    CHECK(durable_cli::detail::same_path(identity.storage_root,expected));
    CHECK(expected!=old_dir);
    installer::ResourceStates pre{};
    CHECK(observer->begin(installer::TransactionOperation::install_or_repair,pre)==S_OK);
    installer::ResourceState desired;desired.present=true;desired.created_by_lifecycle=true;
    desired.owner=L"LiuKai-TSF-v1";desired.path=paths.x64_dll.wstring();desired.threading_model=L"Apartment";
    desired.private_data={0x53,0x49,0x44,0,0x52,0x4f,0x4f,0x54};
    CHECK(observer->before(installer::Resource::machine_com_x64,installer::TransactionMutation::apply,{},desired)==S_OK);
    CHECK(observer->after(installer::Resource::machine_com_x64,installer::TransactionMutation::apply,
        S_OK,S_OK,&desired,true)==S_OK);
    auto final=pre;final[6]=desired;CHECK(observer->commit(final)==S_OK);
    durable::Store journal(expected/L"transaction.journal",identity);
    durable::Document document;CHECK(journal.read(identity,&document)==S_OK&&document.committed);
    CHECK(document.events.size()==5&&document.events[2].intent.resource=="com-machine-x64");
    snapshot::Store snapshot_store(expected/L"transaction.snapshot",identity);
    installer::ResourceStates captured{};CHECK(snapshot_store.read(identity,&captured)==S_OK&&captured==pre);
    auto wrong=identity;wrong.schema_version=2;
    wrong.storage_root=(sandbox/L".liukai-installer-ledger"/L"LiuKai-TSF-v1"/
        files::detail::widen_ascii(identity.token)).wstring();
    CHECK(journal.read(wrong,&document)==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    std::ifstream old(old_path,std::ios::binary);
    const std::string old_bytes(std::istreambuf_iterator<char>(old),{});
    CHECK(old_bytes=="old-scope history must remain immutable");
    observer.reset();
    CHECK(durable_cli::detail::inspect_machine_history(request.install_root,request.owner)==S_OK);
    CHECK(durable_cli::Session::create(request,paths,&observer)==S_OK);
    CHECK(observer->begin(installer::TransactionOperation::install_or_repair,pre)==S_OK);
    CHECK(observer->before(installer::Resource::machine_com_x64,installer::TransactionMutation::apply,{},desired)==S_OK);
    session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    const auto pending_identity=session->identity();
    const auto pending_path=fs::path(pending_identity.storage_root)/L"transaction.journal";
    const auto pending_bytes=files::detail::read_text(pending_path);
    CHECK(!pending_bytes.empty());
    std::unique_ptr<installer::TransactionObserver> rejected;
    CHECK(durable_cli::Session::create(request,paths,&rejected)==HRESULT_FROM_WIN32(ERROR_INVALID_STATE)&&!rejected);
    CHECK(files::detail::read_text(pending_path)==pending_bytes);
    CHECK(durable_cli::detail::inspect_machine_history(request.install_root,request.owner)==HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
    return true;
}


static bool test_machine_uninstall_durable_without_payload_health(const fs::path& sandbox,bool fail_machine) {
    const std::string owner="LiuKai-TSF-v1",version="uninstall-fixture-v1";
    const auto root=sandbox/L"installed";
    const auto version_root=root/L"versions"/files::detail::widen_ascii(version);
    fs::create_directories(version_root);
    CHECK(files::detail::write_text_new(root/L".liukai-owner",files::detail::owner_contents(owner)));
    CHECK(files::detail::write_text_new(root/L".liukai-install-state",
        files::detail::state_contents(owner,version,std::string(64,'a'),std::string(64,'b'))));
    CHECK(files::detail::write_text_new(version_root/L"package-manifest.json","uninstall identity fixture; no healthy DLL required\n"));
    installer::InstallRequest request;request.include_x86=false;request.include_machine_com_x64=true;
    request.owner=L"LiuKai-TSF-v1";request.expected_x64_path=(version_root/L"x64"/L"BoshiamyTsf.dll").wstring();
    request.x64_binary_path=request.expected_x64_path;request.icon_path=(root/L"shared"/L"branding.dll").wstring();
    CHECK(!fs::exists(request.expected_x64_path));
    boshiamy::installer::durable_observer::ApplyProjection projection=[](installer::Resource,
        const installer::ResourceState& state,installer::ResourceState* out)->HRESULT {
        if(!out)return E_POINTER;
        *out=state.present?state:installer::ResourceState{};
        return S_OK;
    };
    std::unique_ptr<installer::TransactionObserver> observer;
    CHECK(durable_cli::Session::create_uninstall(request,root,&observer,projection)==S_OK&&observer);
    auto* session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    const auto identity=session->identity();
    DurableCliBackend backend;
    for(auto resource:{installer::Resource::com_x64,installer::Resource::machine_com_x64,
        installer::Resource::profile,installer::Resource::keyboard_category,
        installer::Resource::system_tray_category,installer::Resource::ui_element_category}) {
        auto& state=backend.states[static_cast<size_t>(resource)];state.present=true;state.created_by_lifecycle=true;
        state.owner=request.owner;
        if(resource==installer::Resource::com_x64||resource==installer::Resource::machine_com_x64) {
            state.path=request.expected_x64_path;state.threading_model=L"Apartment";
        }
        if(resource==installer::Resource::machine_com_x64)state.private_data={1,2,0,3};
        if(resource==installer::Resource::profile)state.path=request.icon_path;
    }
    const auto pre=backend.states;
    if(fail_machine){backend.fail_apply_resource=installer::Resource::machine_com_x64;backend.fail_after_mutating=true;}
    const auto result=installer::uninstall(backend,request,*observer);
    CHECK(result.operation==(fail_machine?E_ACCESSDENIED:S_OK)&&result.cleanup==S_OK);
    installer::ResourceStates after{};CHECK(installer::detail::read_all(backend,after)==S_OK);
    CHECK(session->complete_uninstall(result,after)==S_OK);
    durable::Store journal(fs::path(identity.storage_root)/L"transaction.journal",identity);
    durable::Document document;CHECK(journal.read(identity,&document)==S_OK);
    CHECK(document.events[0].intent.action=="capture-uninstall");
    bool machine_intent=false;
    for(const auto& event:document.events)
        if(event.kind==durable::Event::Kind::intent&&event.intent.resource=="com-machine-x64")machine_intent=true;
    CHECK(machine_intent);
    if(fail_machine) {
        CHECK(!document.committed&&after==pre);
        CHECK(backend.restored==std::vector<installer::Resource>({installer::Resource::machine_com_x64,installer::Resource::com_x64}));
        CHECK(installer::durable_compensation_marker::inspect_registration(identity,root,projection)==S_OK);
        CHECK(fs::is_regular_file(fs::path(identity.storage_root)/L"terminal-registration-compensation.journal"));
    }else {
        CHECK(document.committed&&backend.restored.empty());
        for(const auto& state:after)CHECK(!state.present);
    }
    observer.reset();
    CHECK(durable_cli::detail::inspect_machine_history(root,owner,projection)==S_OK);
    // A stale path is rejected before allocating a new scope ledger.
    request.expected_x64_path=(version_root/L"other"/L"BoshiamyTsf.dll").wstring();
    CHECK(durable_cli::Session::create_uninstall(request,root,&observer,projection)==HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)&&!observer);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    const bool architecture_only=argc==4&&std::wstring(argv[3])==L"--architecture-mismatch";
    if ((argc != 3 && !architecture_only) || !fs::path(argv[1]).is_absolute() || !fs::path(argv[2]).is_absolute()) {
        std::wcerr << L"usage: installer-durable-cli-tests.exe <absolute-package-dir> <absolute-new-sandbox> [--architecture-mismatch]\n";
        return 2;
    }
    const fs::path package_root = argv[1];
    const fs::path sandbox = argv[2];
    std::error_code ec;
    if (fs::exists(sandbox, ec) || ec) {
        std::wcerr << L"test sandbox must not exist\n";
        return 2;
    }
    fs::create_directories(sandbox, ec);
    if (ec) return 2;
    if(architecture_only){
        const bool passed=test_file_phase_fingerprint_v1_compatibility()&&
            test_session_rejects_package_path_architecture_mismatch(package_root,sandbox);
        if(passed)std::cout<<"durable package/path architecture mismatch guards passed before ledger creation\n";
        return passed?0:1;
    }
    const fs::path uninstall_sandbox=sandbox/L"machine-uninstall";
    const fs::path uninstall_failure_sandbox=sandbox/L"machine-uninstall-failure";
    fs::create_directories(uninstall_sandbox,ec);if(ec)return 2;
    fs::create_directories(uninstall_failure_sandbox,ec);if(ec)return 2;
    if(!test_machine_uninstall_durable_without_payload_health(uninstall_sandbox,false)||
       !test_machine_uninstall_durable_without_payload_health(uninstall_failure_sandbox,true))return 1;
    const fs::path machine_sandbox = sandbox / L"machine-scope";
    fs::create_directories(machine_sandbox,ec);if(ec)return 2;
    if(!test_machine_session_namespace_snapshot_and_legacy_history(package_root,machine_sandbox))return 1;
    const fs::path status_sandbox = sandbox / L"status";
    const fs::path session_sandbox = sandbox / L"package-session";
    const fs::path finalize_sandbox = sandbox / L"commit-before-finalize";
    const fs::path retry_sandbox = sandbox / L"idempotent-retry";
    const fs::path drift_sandbox = sandbox / L"manifest-drift";
    const fs::path architecture_sandbox = sandbox / L"architecture-mismatch";
    fs::create_directories(status_sandbox, ec);
    if (ec) return 2;
    fs::create_directories(session_sandbox, ec);
    if (ec) return 2;
    fs::create_directories(finalize_sandbox, ec);
    if (ec) return 2;
    fs::create_directories(retry_sandbox, ec);
    if (ec) return 2;
    fs::create_directories(drift_sandbox, ec);
    if (ec) return 2;
    fs::create_directories(architecture_sandbox, ec);
    if (ec) return 2;
    if (!test_status_does_not_create_ledger(status_sandbox) ||
        !test_file_phase_fingerprint_v1_compatibility() ||
        !test_package_command_creates_one_committed_external_session(package_root, session_sandbox) ||
        !test_factory_commit_precedes_file_finalize(package_root, finalize_sandbox) ||
        !test_identical_package_retry_is_idempotent(package_root, retry_sandbox) ||
        !test_session_rejects_package_path_architecture_mismatch(package_root,architecture_sandbox) ||
        !test_manifest_drift_after_prepare_refuses_package_deploy(package_root, drift_sandbox)) return 1;
    std::cout << "durable package CLI session assertions passed; fake backend only\n";
    return 0;
}
