#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "../installer/deployment_coordinator.hpp"
#include "../installer/durable_cli_session.hpp"
#include "../installer/durable_deployment_recovery.hpp"

#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << "\n"; return false; } } while (0)
namespace fs = std::filesystem;
namespace installer = boshiamy::installer;
namespace files = boshiamy::installer::files;
namespace durable_cli = boshiamy::installer::durable_cli;
namespace recovery = boshiamy::installer::durable_recovery;

struct FakeBackend final : installer::Backend {
    installer::ResourceStates states{};
    size_t writes = 0;
    installer::Resource fail_read = installer::Resource::count;
    HRESULT read_result = S_OK;
    HRESULT health_result = S_OK;
    size_t health_calls = 0;
    installer::Resource fail_apply=installer::Resource::count;
    bool partial_failure=false;
    HRESULT prepare_apply_targets(installer::TransactionOperation,const installer::InstallRequest& request,
            const installer::ResourceStates&,installer::ResourceStates* desired) override {
        if (!desired) return E_POINTER;
        auto& machine=(*desired)[static_cast<size_t>(installer::Resource::machine_com_x64)];
        if (request.include_machine_com_x64 && machine.present && machine.private_data.empty())
            machine.private_data={0x53,0x49,0x44,0x72,0x6f,0x6f,0x74,0xff};
        return S_OK;
    }
    HRESULT project_apply_state(installer::Resource resource,const installer::ResourceState& state,
            installer::ResourceState* out) const override {
        if (!out) return E_POINTER;
        *out=state;
        if (resource==installer::Resource::machine_com_x64) {
            if (!state.present) {*out={};return S_OK;}
            if (state.private_data.size()<7 || state.private_data[0]!=0x53) return E_INVALIDARG;
            out->private_data.resize(7); // fake canonical SID/root, excluding raw metadata
        }
        return S_OK;
    }
    HRESULT read(installer::Resource resource, installer::ResourceState* state) override {
        if (!state || resource == installer::Resource::count) return E_POINTER;
        if (resource == fail_read) return read_result;
        *state = states[static_cast<size_t>(resource)];
        return S_OK;
    }
    HRESULT apply(installer::Resource resource, const installer::ResourceState& state) override {
        ++writes;
        if (resource==fail_apply) {
            if (partial_failure) states[static_cast<size_t>(resource)] = state;
            return E_ACCESSDENIED;
        }
        states[static_cast<size_t>(resource)] = state;
        return S_OK;
    }
    HRESULT restore(installer::Resource resource, const installer::ResourceState& state) override {
        ++writes;
        states[static_cast<size_t>(resource)] = state;
        return S_OK;
    }
    HRESULT verify_install_health() override {
        ++health_calls;
        return health_result;
    }
};

static bool make_committed(const fs::path& package, const fs::path& sandbox,
                          durable_cli::journal::Identity* identity,
                          FakeBackend* backend, int x86_kind = 1, bool machine = false) {
    files::DeploymentRequest request;
    request.package_root = package;
    request.install_root = sandbox / L"install";
    request.owner = "LiuKai-TSF-v1";
    request.include_machine_com_x64=machine;
    if (x86_kind != 0) {
        auto& x86 = backend->states[static_cast<size_t>(installer::Resource::com_x86)];
        x86.present = true;
        x86.owner = x86_kind == 2 ? L"LiuKai-TSF-v1" : L"Legacy-TSF";
        x86.path = x86_kind == 2 ? L"C:\\legacy-owned\\BoshiamyTsf.dll" : L"C:\\legacy\\BoshiamyTsf.dll";
        x86.threading_model = L"Apartment";
        x86.private_data = {0x13, 0x37, 0x00, 0x42};
    }
    installer::DeploymentObserverFactory factory = [&](const files::DeploymentRequest& req,
            const files::DeploymentPaths& paths,
            std::unique_ptr<installer::TransactionObserver>* output) -> HRESULT {
        const HRESULT status = durable_cli::Session::create(req, paths, output,
            [backend](installer::Resource resource,const installer::ResourceState& state,installer::ResourceState* projected) {
                return backend->project_apply_state(resource,state,projected);
            });
        if (SUCCEEDED(status)) {
            auto* session = dynamic_cast<durable_cli::Session*>(output->get());
            if (!session) return E_UNEXPECTED;
            *identity = session->identity();
        }
        return status;
    };
    const auto result = installer::deploy_and_register(*backend, request, nullptr, factory);
    return result.operation == S_OK && result.cleanup == S_OK && identity->schema_version == (machine?3u:2u);
}

static bool test_singleton_commit_classification(const fs::path& package,
                                                 const fs::path& sandbox) {
    durable_cli::journal::Identity identity;
    FakeBackend backend;
    CHECK(make_committed(package, sandbox, &identity, &backend));
    CHECK(!fs::exists(fs::path(identity.install_root) /
                      (L".txn-" + std::wstring(identity.token.begin(), identity.token.end()))));
    CHECK(backend.states[static_cast<size_t>(installer::Resource::com_x86)].owner == L"Legacy-TSF");
    CHECK(backend.writes > 0);
    const size_t writes_after_install = backend.writes;

    const auto accepted = recovery::inspect_committed_x64_singleton(identity, backend);
    CHECK(accepted.classification == recovery::Classification::committed_x64_singleton_coherent);
    CHECK(accepted.status == S_OK && !accepted.automatic_action && !accepted.filesystem_mutated &&
          !accepted.backend_mutated && backend.writes == writes_after_install);
    CHECK(!accepted.current_files.has_x86);
    CHECK(backend.health_calls == 2); // install check + recovery classification

    auto altered = backend;
    altered.states[static_cast<size_t>(installer::Resource::com_x86)].private_data[0] ^= 0xff;
    auto report = recovery::inspect_committed_x64_singleton(identity, altered);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
    CHECK(altered.writes == writes_after_install);

    altered = backend;
    altered.states[static_cast<size_t>(installer::Resource::com_x86)].path = L"C:\\foreign\\changed.dll";
    report = recovery::inspect_committed_x64_singleton(identity, altered);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);

    altered = backend;
    altered.states[static_cast<size_t>(installer::Resource::com_x64)].path += L".changed";
    report = recovery::inspect_committed_x64_singleton(identity, altered);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
    CHECK(altered.writes == writes_after_install);

    altered = backend;
    altered.fail_read = installer::Resource::com_x86;
    altered.read_result = E_ACCESSDENIED;
    report = recovery::inspect_committed_x64_singleton(identity, altered);
    CHECK(report.classification == recovery::Classification::unreadable);
    CHECK(report.status == E_ACCESSDENIED && altered.writes == writes_after_install);

    altered = backend;
    altered.fail_read = installer::Resource::com_x86;
    altered.read_result = S_FALSE;
    report = recovery::inspect_committed_x64_singleton(identity, altered);
    CHECK(report.classification == recovery::Classification::unreadable);
    CHECK(report.status == S_FALSE && altered.writes == writes_after_install);

    for (const HRESULT health_failure : {E_ACCESSDENIED, E_NOTIMPL, S_FALSE}) {
        altered = backend;
        altered.health_result = health_failure;
        report = recovery::inspect_committed_x64_singleton(identity, altered);
        CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
        CHECK(report.status == health_failure && altered.health_calls == backend.health_calls + 1);
        CHECK(altered.writes == writes_after_install);
    }

    durable_cli::journal::Document committed_document;
    durable_cli::journal::Store journal_store(fs::path(identity.storage_root) / L"transaction.journal",
                                              identity, fs::path(identity.storage_root));
    CHECK(journal_store.read(identity, &committed_document) == S_OK);
    altered = backend;
    altered.states[static_cast<size_t>(installer::Resource::com_x64)].path += L".wrong-target";
    std::string recomputed_wrong_commit;
    CHECK(recovery::detail::full_state_fingerprint(identity, altered.states, &recomputed_wrong_commit));
    committed_document.events.back().commit_fingerprint = recomputed_wrong_commit;
    CHECK(recovery::detail::committed_singleton_events_valid(committed_document,
        committed_document.events[1].observation.observed_fingerprint));
    CHECK(!recovery::detail::committed_singleton_state_contract(identity, accepted.current_files,
        accepted.pre_state, altered.states));

    const fs::path install_root(identity.install_root);
    const fs::path duplicate_txn = install_root / L".txn-foreign";
    CHECK(fs::create_directory(duplicate_txn));
    report = recovery::inspect_committed_x64_singleton(identity, backend);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
    CHECK(fs::remove(duplicate_txn));

    const fs::path snapshot_path = fs::path(identity.storage_root) / L"transaction.snapshot";
    const fs::path hidden_snapshot = snapshot_path.string() + ".hidden";
    std::error_code file_ec;
    fs::rename(snapshot_path, hidden_snapshot, file_ec);
    CHECK(!file_ec);
    report = recovery::inspect_committed_x64_singleton(identity, backend);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
    fs::rename(hidden_snapshot, snapshot_path, file_ec);
    CHECK(!file_ec);

    auto wrong_identity = identity;
    wrong_identity.token = wrong_identity.token == "a" ? "b" : "a";
    wrong_identity.storage_root = (fs::path(identity.storage_root).parent_path() /
        fs::path(std::wstring(wrong_identity.token.begin(), wrong_identity.token.end()))).wstring();
    report = recovery::inspect_committed_x64_singleton(wrong_identity, backend);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);

    wrong_identity = identity;
    wrong_identity.package_manifest_sha256[0] = wrong_identity.package_manifest_sha256[0] == '0' ? '1' : '0';
    report = recovery::inspect_committed_x64_singleton(wrong_identity, backend);
    CHECK(report.classification == recovery::Classification::incomplete_or_ambiguous);
    return true;
}

static bool test_owned_and_absent_com32(const fs::path& package, const fs::path& sandbox) {
    for (const int x86_kind : {0, 2}) {
        const fs::path child = sandbox / (x86_kind == 0 ? L"absent-com32" : L"old-owned-com32");
        std::error_code ec;
        CHECK(fs::create_directory(child, ec) && !ec);
        durable_cli::journal::Identity identity;
        FakeBackend backend;
        CHECK(make_committed(package, child, &identity, &backend, x86_kind));
        const auto report = recovery::inspect_committed_x64_singleton(identity, backend);
        CHECK(report.classification == recovery::Classification::committed_x64_singleton_coherent);
        CHECK(report.status == S_OK && backend.writes > 0);
        const auto& current_x86 = backend.states[static_cast<size_t>(installer::Resource::com_x86)];
        CHECK(current_x86 == report.pre_state[static_cast<size_t>(installer::Resource::com_x86)]);
        if (x86_kind == 0) CHECK(!current_x86.present);
        else CHECK(current_x86.present && current_x86.owner == L"LiuKai-TSF-v1" &&
                   current_x86.path == L"C:\\legacy-owned\\BoshiamyTsf.dll");
    }
    return true;
}

static bool test_machine_file_publication_boundary(const fs::path& package,const fs::path& sandbox) {
    CHECK(fs::create_directory(sandbox));
    files::DeploymentRequest request;request.package_root=package;request.install_root=sandbox/L"install";
    request.owner="LiuKai-TSF-v1";request.include_machine_com_x64=true;
    std::unique_ptr<files::PreparedDeployment> deployment;
    CHECK(files::FileDeployment::prepare(request,&deployment)==S_OK);
    std::unique_ptr<installer::TransactionObserver> observer;
    CHECK(durable_cli::Session::create(request,deployment->paths(),&observer)==S_OK);
    auto* session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    const auto identity=session->identity();
    CHECK(deployment->bind_transaction_token(session->file_transaction_token())==S_OK);
    installer::ResourceStates pre{};
    CHECK(session->before_file_publication(pre,nullptr,deployment->paths())==S_OK);
    CHECK(deployment->apply()==S_OK);
    files::DeploymentSnapshot current;
    CHECK(files::FileDeployment::inspect(request.install_root,request.owner,&current)==S_OK);
    CHECK(session->after_file_publication(S_OK,&current)==S_OK);
    observer.reset();
    FakeBackend backend;
    auto report=recovery::inspect_file_published_before_tsf(identity,backend);
    CHECK(report.classification==recovery::Classification::file_published_tsf_unchanged && report.status==S_OK && backend.writes==0);
    backend.states[static_cast<size_t>(installer::Resource::machine_com_x64)]={true,true,L"Other",L"C:\\foreign.dll",L"Apartment",{1}};
    report=recovery::inspect_file_published_before_tsf(identity,backend);
    CHECK(report.classification==recovery::Classification::incomplete_or_ambiguous && backend.writes==0);
    return true;
}

static bool test_machine_recovery_and_compensation(const fs::path& package,const fs::path& sandbox) {
    CHECK(fs::create_directory(sandbox));
    durable_cli::journal::Identity identity;FakeBackend backend;
    CHECK(make_committed(package,sandbox,&identity,&backend,1,true));
    const auto projection=[&backend](installer::Resource resource,const installer::ResourceState& state,installer::ResourceState* out) {
        return backend.project_apply_state(resource,state,out);
    };
    const size_t machine=static_cast<size_t>(installer::Resource::machine_com_x64);
    const auto installed=backend.states;
    const size_t writes=backend.writes;
    auto report=recovery::inspect_committed_x64_singleton(identity,backend);
    CHECK(report.classification==recovery::Classification::committed_x64_singleton_coherent && backend.writes==writes);
    auto drift=backend;drift.states[machine].private_data.back()^=0x01;
    CHECK(recovery::inspect_committed_x64_singleton(identity,drift).classification==recovery::Classification::incomplete_or_ambiguous);
    drift=backend;drift.states[machine].path+=L".foreign";
    CHECK(recovery::inspect_committed_x64_singleton(identity,drift).classification==recovery::Classification::incomplete_or_ambiguous);
    auto wrong_scope=identity;wrong_scope.schema_version=2;
    CHECK(recovery::inspect_committed_x64_singleton(wrong_scope,backend).status!=S_OK);

    // A compensated package phase has a scope-bound terminal file proof and
    // leaves the exact raw machine prestate intact.
    files::DeploymentSnapshot current;
    CHECK(files::FileDeployment::inspect(identity.install_root,identity.owner,&current)==S_OK);
    files::DeploymentRequest request;request.package_root=package;request.install_root=identity.install_root;
    request.owner=identity.owner;request.include_machine_com_x64=true;
    request.expected_old_version=current.version;request.expected_old_cli_sha256=current.installer_cli_sha256;
    request.expected_old_branding_sha256=current.branding_sha256;
    durable_cli::journal::Identity failed_identity;
    const installer::DeploymentObserverFactory factory=[&](const files::DeploymentRequest& input,const files::DeploymentPaths& paths,
            std::unique_ptr<installer::TransactionObserver>* out) {
        const HRESULT status=durable_cli::Session::create(input,paths,out,projection);
        if(status==S_OK)failed_identity=dynamic_cast<durable_cli::Session*>(out->get())->identity();
        return status;
    };
    const installer::RegistrationExecutor reject=[](installer::Backend&,const installer::InstallRequest&,installer::TransactionObserver*) {
        return installer::LifecycleResult{E_FAIL,S_OK};
    };
    const auto rejected=installer::deploy_and_register(backend,request,nullptr,factory,reject);
    CHECK(rejected.operation==E_FAIL && rejected.cleanup==S_OK && backend.states==installed);
    CHECK(installer::durable_compensation_marker::inspect(failed_identity,request.install_root,projection)==S_OK);

    installer::InstallRequest remove;
    remove.owner=L"LiuKai-TSF-v1";remove.include_x86=false;remove.include_machine_com_x64=true;
    remove.expected_x64_path=current.x64_dll.wstring();
    std::unique_ptr<installer::TransactionObserver> observer;
    CHECK(durable_cli::Session::create_uninstall(remove,request.install_root,&observer,projection)==S_OK);
    auto* session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    const auto uninstall_identity=session->identity();
    backend.fail_apply=installer::Resource::machine_com_x64;backend.partial_failure=true;
    const auto failed=installer::uninstall(backend,remove,observer.get());
    CHECK(failed.operation==E_ACCESSDENIED && failed.cleanup==S_OK && backend.states==installed);
    CHECK(session->complete_uninstall(failed,backend.states)==S_OK);
    observer.reset();
    CHECK(installer::durable_compensation_marker::inspect_registration(uninstall_identity,request.install_root,projection)==S_OK);
    CHECK(installer::durable_compensation_marker::inspect_registration(uninstall_identity,request.install_root)!=S_OK);
    CHECK(installer::durable_compensation_marker::record_registration(uninstall_identity,{},projection)!=S_OK);
    CHECK(installer::durable_compensation_marker::inspect(uninstall_identity,request.install_root,projection)!=S_OK);

    // Closed compensation permits a fresh removal; unresolved state cannot be
    // confused with either terminal format. Removal does not call install health.
    CHECK(durable_cli::Session::create_uninstall(remove,request.install_root,&observer,projection)==S_OK);
    session=dynamic_cast<durable_cli::Session*>(observer.get());CHECK(session);
    backend.fail_apply=installer::Resource::count;backend.health_result=E_ACCESSDENIED;
    const size_t health=backend.health_calls;
    const auto removed=installer::uninstall(backend,remove,observer.get());
    CHECK(removed.operation==S_OK && removed.cleanup==S_OK && backend.health_calls==health);
    CHECK(session->complete_uninstall(removed,backend.states)==S_OK);
    CHECK(backend.states[machine]==installer::ResourceState{} && backend.states[0]==installer::ResourceState{});
    CHECK(backend.states[1]==installed[1]);
    observer.reset();
    // An old terminal file proof remains inspectable after the live files have
    // advanced or become unhealthy. Minting a new proof still demands exact
    // live file restoration.
    {std::ofstream append(current.installer_cli,std::ios::binary|std::ios::app);append.put('x');}
    CHECK(installer::durable_compensation_marker::inspect(failed_identity,request.install_root,projection)==S_OK);
    installer::durable_compensation_marker::Facts facts;
    CHECK(installer::durable_compensation_marker::collect(failed_identity,request.install_root,&facts,projection)!=S_OK);
    const fs::path marker=fs::path(uninstall_identity.storage_root)/L"terminal-registration-compensation.journal";
    {std::ofstream append(marker,std::ios::binary|std::ios::app);append.put('x');}
    CHECK(installer::durable_compensation_marker::inspect_registration(uninstall_identity,request.install_root,projection)!=S_OK);
    CHECK(durable_cli::Session::create_uninstall(remove,request.install_root,&observer,projection)!=S_OK);
    return true;
}

static bool test_event_sequence_guards() {
    using Event = durable_cli::journal::Event;
    durable_cli::journal::Document document;
    document.committed = true;
    document.all_operations_succeeded = true;
    const std::string pre(64, '1');
    Event intent; intent.kind = Event::Kind::intent;
    intent.intent.sequence = 1; intent.intent.resource = "prestate";
    intent.intent.action = "capture-install-or-repair";
    intent.intent.expected_before_fingerprint.assign(64, '0');
    intent.intent.desired_fingerprint = pre;
    Event observation; observation.kind = Event::Kind::observation;
    observation.observation.sequence = 1; observation.observation.operation_status = S_OK;
    observation.observation.observed_fingerprint = pre;
    Event file_intent; file_intent.kind = Event::Kind::intent;
    file_intent.intent.sequence = 2; file_intent.intent.resource = "file-publication";
    file_intent.intent.action = "apply"; file_intent.intent.desired_fingerprint.assign(64, '2');
    Event file_observation; file_observation.kind = Event::Kind::observation;
    file_observation.observation.sequence = 2; file_observation.observation.operation_status = S_OK;
    file_observation.observation.observed_fingerprint = file_intent.intent.desired_fingerprint;
    Event commit; commit.kind = Event::Kind::commit; commit.commit_fingerprint.assign(64, '3');
    document.events = {intent, observation, file_intent, file_observation, commit};
    CHECK(recovery::detail::committed_singleton_events_valid(document, pre));

    Event x86_intent; x86_intent.kind = Event::Kind::intent;
    x86_intent.intent.sequence = 3; x86_intent.intent.resource = "com-x86";
    x86_intent.intent.action = "apply"; x86_intent.intent.desired_fingerprint.assign(64, '4');
    Event x86_observation; x86_observation.kind = Event::Kind::observation;
    x86_observation.observation.sequence = 3; x86_observation.observation.operation_status = S_OK;
    x86_observation.observation.observed_fingerprint = x86_intent.intent.desired_fingerprint;
    document.events = {intent, observation, file_intent, file_observation, x86_intent, x86_observation, commit};
    CHECK(!recovery::detail::committed_singleton_events_valid(document, pre));
    document.events.pop_back();
    CHECK(!recovery::detail::committed_singleton_events_valid(document, pre));
    document.events = {intent, observation, file_intent, file_observation, commit};
    document.events[1].observation.operation_status = E_ACCESSDENIED;
    CHECK(!recovery::detail::committed_singleton_events_valid(document, pre));
    document.events = {intent, observation, file_intent, file_observation, commit};
    document.events[0].intent.desired_fingerprint.assign(64, '9');
    CHECK(!recovery::detail::committed_singleton_events_valid(document, pre));
    return true;
}

int wmain(int argc, wchar_t** argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc != 3 || !fs::path(argv[1]).is_absolute() || !fs::path(argv[2]).is_absolute()) {
        std::wcerr << L"usage: installer-durable-singleton-recovery-tests.exe <absolute-x64-package-dir> <absolute-new-sandbox>\n";
        return 2;
    }
    const fs::path package = argv[1], sandbox = argv[2];
    std::error_code ec;
    files::detail::Package parsed;
    if (!files::detail::parse_package(package, &parsed) || parsed.has_x86 ||
        fs::exists(sandbox, ec) || ec) return 2;
    fs::create_directories(sandbox, ec);
    if (ec) return 2;
    if (!test_event_sequence_guards() || !test_singleton_commit_classification(package, sandbox) ||
        !test_owned_and_absent_com32(package, sandbox) ||
        !test_machine_recovery_and_compensation(package,sandbox/L"machine-scope") ||
        !test_machine_file_publication_boundary(package,sandbox/L"machine-prefile")) return 1;
    std::cout << "committed x64 singleton recovery classification assertions passed\n";
    return 0;
}
