#include "../installer/dev_host_migration.hpp"
#include "../installer/durable_transaction_observer.hpp"
#include <iostream>
using namespace boshiamy::installer;
namespace migration = boshiamy::installer::dev_host_migration;
namespace journal = boshiamy::installer::durable_journal;
namespace snapshot = boshiamy::installer::durable_snapshot;
namespace durable = boshiamy::installer::durable_observer;
#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)
struct Fake final : Backend {
    ResourceStates states{};
    ResourceState trusted;
    unsigned round = 0, health = 0;
    std::vector<Resource> applies, restores;
    HRESULT apply_status = S_OK, restore_status = S_OK, verify_status = S_OK;
    bool no_apply_effect = false, no_restore_effect = false, prove_after = true;
    unsigned failed_round = 0;
    Resource failed_resource = Resource::count;
    HRESULT read_status = S_FALSE;
    std::function<void(Resource, unsigned)> on_read;
    std::function<void()> on_apply;
    HRESULT read(Resource r, ResourceState* out) override {
        if (!out) return E_POINTER;
        if (r == Resource::com_x64) ++round;
        if (on_read) on_read(r, round);
        if (round == failed_round && r == failed_resource) return read_status;
        *out = states[static_cast<size_t>(r)];
        return S_OK;
    }
    HRESULT apply(Resource r, const ResourceState& target) override {
        applies.push_back(r);
        if (r != Resource::com_x64) return E_ACCESSDENIED;
        if (!no_apply_effect) {
            states[0] = target;
            states[0].private_data = {9, 8, 7, 0}; // Backend-generated complete new opaque snapshot.
        }
        trusted = states[0];
        if (on_apply) on_apply();
        return apply_status;
    }
    HRESULT restore(Resource r, const ResourceState& original) override {
        restores.push_back(r);
        if (r != Resource::com_x64) return E_ACCESSDENIED;
        if (!no_restore_effect) states[0] = original;
        return restore_status;
    }
    HRESULT verify_applied(Resource r, const ResourceState& target, const ResourceState& observed) const override {
        if (verify_status != S_OK) return verify_status;
        if (!prove_after || r != Resource::com_x64 || observed != trusted ||
            observed.path != target.path || observed.owner != target.owner ||
            observed.threading_model != target.threading_model) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        return S_OK;
    }
    HRESULT verify_install_health() override { ++health; return E_FAIL; }
};
InstallRequest request() {
    InstallRequest r;
    r.owner = L"LiuKai-TSF-v1";
    r.include_x86 = false;
    r.expected_x64_path = L"C:\\dev\\r29\\BoshiamyTsf.dll";
    r.x64_binary_path = L"C:\\managed\\versions\\r30\\x64\\BoshiamyTsf.dll";
    r.icon_path = L"C:\\managed\\shared\\branding.dll";
    return r;
}
ResourceStates original() {
    ResourceStates s{};
    s[0] = {true, true, request().owner, request().expected_x64_path, L"Apartment", {1, 0, 255, 6}};
    s[1] = {true, false, L"Foreign-COM32", L"C:\\foreign\\tip.dll", L"Both", {255, 0, 77}};
    s[2] = {true, false, L"Borrowed-profile", L"", L"", {4, 0, 3, 255}};
    s[3].private_data = {1, 2, 3};
    s[4].private_data = {5, 0, 4};
    return s;
}
struct Fixture {
    Fake backend;
    journal::Identity id;
    std::unique_ptr<journal::Store> log;
    std::unique_ptr<snapshot::Store> sidecar;
    std::unique_ptr<durable::Observer> observer;
    explicit Fixture(const std::filesystem::path& root) {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture exists");
        backend.states = original();
        id.owner = "LiuKai-TSF-v1"; id.token = "dev-migrate"; id.install_root = root.wstring();
        id.package_version = "fixture"; id.package_manifest_sha256 = std::string(64, 'a');
        log = std::make_unique<journal::Store>(root / L"journal", id);
        sidecar = std::make_unique<snapshot::Store>(root / L"snapshot", id);
        observer = std::make_unique<durable::Observer>(*log, *sidecar, id);
    }
};
struct BeforeRestore final : TransactionObserver {
    TransactionObserver& inner;
    std::function<void()> change;
    BeforeRestore(TransactionObserver& value, std::function<void()> mutation) : inner(value), change(std::move(mutation)) {}
    HRESULT begin(TransactionOperation op, const ResourceStates& s) override { return inner.begin(op, s); }
    HRESULT before(Resource r, TransactionMutation m, const ResourceState& a, const ResourceState& b) override {
        HRESULT hr = inner.before(r, m, a, b);
        if (hr == S_OK && m == TransactionMutation::restore) change();
        return hr;
    }
    HRESULT after(Resource r, TransactionMutation m, HRESULT op, HRESULT rd, const ResourceState* s, bool may) override {
        return inner.after(r, m, op, rd, s, may);
    }
};
struct CallbackFailure final : TransactionObserver {
    TransactionObserver& inner;
    bool after_write;
    HRESULT injected;
    CallbackFailure(TransactionObserver& value, bool after, HRESULT status) : inner(value), after_write(after), injected(status) {}
    HRESULT begin(TransactionOperation op, const ResourceStates& states) override { return inner.begin(op, states); }
    HRESULT before(Resource r, TransactionMutation m, const ResourceState& a, const ResourceState& b) override {
        if (!after_write && m == TransactionMutation::apply) return injected;
        return inner.before(r, m, a, b);
    }
    HRESULT after(Resource r, TransactionMutation m, HRESULT op, HRESULT rd, const ResourceState* value, bool may) override {
        const HRESULT logged = inner.after(r, m, op, rd, value, may);
        return logged != S_OK ? logged : after_write && m == TransactionMutation::apply ? injected : S_OK;
    }
};
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    CHECK(std::filesystem::create_directory(root));
    {
        Fixture f(root / "success");
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        std::cout << "success operation=" << static_cast<unsigned long>(result.operation)
                  << " cleanup=" << static_cast<unsigned long>(result.cleanup) << '\n';
        CHECK(result.operation == S_OK && result.cleanup == S_OK);
        CHECK(f.backend.applies == std::vector<Resource>{Resource::com_x64} && f.backend.restores.empty());
        CHECK(f.backend.health == 0 && f.backend.states[0].path == request().x64_binary_path);
        for (size_t i = 1; i < original().size(); ++i) CHECK(f.backend.states[i] == original()[i]);
        journal::Document document;
        CHECK(f.log->read(f.id, &document) == S_OK && !document.committed && document.all_operations_succeeded);
        CHECK(document.events[0].intent.action == "capture-x64-dev-host-migration");
        CHECK(f.observer->commit(f.backend.states) == S_OK);
    }
    for (unsigned which = 0; which != 13; ++which) {
        Fixture f(root / ("invalid-" + std::to_string(which)));
        auto r = request(); auto before = original();
        if (which == 0) r.include_x86 = true;
        if (which == 1) r.expected_x64_path.clear();
        if (which == 2) r.x64_binary_path = r.expected_x64_path;
        if (which == 3) before[0].owner = L"foreign";
        if (which == 4) before[0].threading_model = L"Both";
        if (which == 5) before[2].present = false;
        if (which == 6) before[0].created_by_lifecycle = false;
        if (which == 7) r.expected_x64_path = L"relative.dll";
        if (which == 8) r.x64_binary_path = L"relative.dll";
        if (which == 9) r.x64_binary_path.push_back(L'\0');
        if (which == 10) r.expected_x86_path = L"C:\\foreign\\tip.dll";
        if (which == 11) r.include_machine_com_x64 = true;
        if (which == 12) before[static_cast<size_t>(Resource::machine_com_x64)].present = true;
        f.backend.states = before;
        CHECK(migration::execute(f.backend, r, before, f.observer.get()).operation != S_OK);
        CHECK(f.backend.applies.empty() && f.backend.restores.empty());
    }
    {
        Fixture f(root / "initial-drift"); f.backend.states[1].private_data.push_back(9);
        CHECK(migration::execute(f.backend, request(), original(), f.observer.get()).operation != S_OK);
        CHECK(f.backend.applies.empty() && f.backend.restores.empty());
    }
    for (unsigned round : {2u, 3u, 4u}) {
        Fixture f(root / ("other-drift-" + std::to_string(round)));
        f.backend.on_read = [&](Resource r, unsigned n) { if (n == round && r == Resource::com_x86) f.backend.states[1].private_data.push_back(42); };
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation != S_OK && result.cleanup != S_OK && f.backend.restores.empty());
        CHECK(f.backend.applies.size() == (round == 2 ? 0u : 1u));
        CHECK(f.backend.states[1].private_data.back() == 42);
    }
    for (unsigned round : {1u, 2u, 4u}) {
        Fixture f(root / ("unknown-round-" + std::to_string(round)));
        f.backend.failed_round = round; f.backend.failed_resource = Resource::profile;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == S_FALSE && result.cleanup != S_OK && f.backend.restores.empty());
        CHECK(f.backend.applies.size() == (round == 4 ? 1u : 0u));
    }
    {
        Fixture f(root / "final-com-drift");
        f.backend.on_read = [&](Resource r, unsigned n) { if (n == 4 && r == Resource::com_x64) f.backend.states[0].private_data.push_back(99); };
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation != S_OK && result.cleanup != S_OK && f.backend.restores.empty());
        CHECK(f.backend.states[0].private_data.back() == 99);
    }
    for (bool after : {false, true}) {
        Fixture f(root / (after ? "callback-after" : "callback-before"));
        CallbackFailure observer(*f.observer, after, S_FALSE);
        auto result = migration::execute(f.backend, request(), original(), &observer);
        CHECK(result.operation == S_FALSE && f.backend.restores.empty());
        CHECK(f.backend.applies.size() == (after ? 1u : 0u));
        if (after) CHECK(result.cleanup == S_FALSE);
    }
    for (HRESULT hr : {S_FALSE, E_ACCESSDENIED}) {
        Fixture f(root / (hr == S_FALSE ? "unknown-positive" : "unknown-negative"));
        f.backend.failed_round = 3; f.backend.failed_resource = Resource::profile; f.backend.read_status = hr;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == hr && result.cleanup != S_OK && f.backend.restores.empty());
        CHECK(f.backend.states[0].path == request().x64_binary_path);
    }
    {
        Fixture f(root / "false-apply"); f.backend.apply_status = S_FALSE; f.backend.no_apply_effect = true;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == S_FALSE && result.cleanup == S_OK && f.backend.states == original());
    }
    {
        Fixture f(root / "after-effect"); f.backend.apply_status = E_ACCESSDENIED;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == E_ACCESSDENIED && result.cleanup == S_OK && f.backend.states == original());
        CHECK(f.backend.restores == std::vector<Resource>{Resource::com_x64});
    }
    {
        Fixture f(root / "unproven-partial"); f.backend.apply_status = E_ACCESSDENIED; f.backend.prove_after = false;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == E_ACCESSDENIED && result.cleanup != S_OK && f.backend.restores.empty());
    }
    {
        Fixture f(root / "false-verify"); f.backend.verify_status = S_FALSE;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == S_FALSE && result.cleanup != S_OK && f.backend.restores.empty());
    }
    {
        Fixture f(root / "foreign-com"); f.backend.on_apply = [&] { f.backend.states[0].private_data.push_back(0x42); };
        CHECK(migration::execute(f.backend, request(), original(), f.observer.get()).operation != S_OK);
        CHECK(f.backend.restores.empty() && f.backend.states[0].private_data.back() == 0x42);
    }
    {
        Fixture f(root / "rollback-guard"); f.backend.apply_status = E_ACCESSDENIED;
        BeforeRestore observer(*f.observer, [&] { f.backend.states[2].private_data.push_back(77); });
        auto result = migration::execute(f.backend, request(), original(), &observer);
        CHECK(result.operation == E_ACCESSDENIED && result.cleanup != S_OK && f.backend.restores.empty());
        CHECK(f.backend.states[2].private_data.back() == 77);
    }
    {
        Fixture f(root / "false-restore"); f.backend.apply_status = E_ACCESSDENIED;
        f.backend.restore_status = S_FALSE; f.backend.no_restore_effect = true;
        auto result = migration::execute(f.backend, request(), original(), f.observer.get());
        CHECK(result.operation == E_ACCESSDENIED && result.cleanup == S_FALSE);
        CHECK(f.backend.states[0].path == request().x64_binary_path);
    }
    std::cout << "x64-only COM migration guards, durable deferred commit and exact inverse passed; fake backend only\n";
}
