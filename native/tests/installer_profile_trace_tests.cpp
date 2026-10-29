// Exercise the production store and JSON serializer with in-memory dependencies.
// No COM initialization, registry access, installation, or real entry point.
#define wmain installer_profile_trace_unused_entry
#include "../installer/lifecycle_main.cpp"
#undef wmain
#include <sstream>

using namespace boshiamy::installer;
#define CHECK(x) do { if (!(x)) { std::cerr << #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

enum class Failure { none, registration, registration_after_effect, enable_ignored, icon, user_read };
struct FakeProfileApi final : TsfSharedApi {
    Failure failure;
    bool present = false, enabled = false;
    bool hide_created_profile = false;
    bool category = false, hide_created_category = false, category_fails_after_create = false;
    std::wstring icon;
    std::vector<std::string> calls;
    explicit FakeProfileApi(Failure value) : failure(value) {}
    HRESULT profile_present(bool* out) override {
        calls.push_back("profile_present"); *out = present && !hide_created_profile; return S_OK;
    }
    HRESULT profile_icon_path(std::wstring* out) override {
        calls.push_back("profile_icon_path"); *out = icon;
        return failure == Failure::icon ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK;
    }
    HRESULT register_profile(const std::wstring& path, bool by_default) override {
        calls.push_back(by_default ? "register_enabled" : "register_disabled");
        if (failure == Failure::registration) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (failure == Failure::registration_after_effect) {
            present = true; icon = path;
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        present = true; enabled = by_default; icon = path; return S_OK;
    }
    HRESULT unregister_profile() override {
        calls.push_back("unregister_profile"); present = false; return S_OK;
    }
    HRESULT profile_user_state(bool* out, bool* is_default) override {
        calls.push_back("profile_user_state");
        if (failure == Failure::user_read) return E_ACCESSDENIED;
        *out = enabled; *is_default = false; return S_OK;
    }
    HRESULT restore_profile_user_state(bool value, bool) override {
        calls.push_back(value ? "enable_true" : "enable_false");
        if (failure != Failure::enable_ignored) enabled = value;
        return S_OK;
    }
    HRESULT category_present(Resource resource, bool* out) override {
        if (resource != Resource::keyboard_category || !out) return E_INVALIDARG;
        *out = category && !hide_created_category; return S_OK;
    }
    HRESULT register_category(Resource resource) override {
        if (resource != Resource::keyboard_category) return E_INVALIDARG;
        category = true; return category_fails_after_create ? E_ACCESSDENIED : S_OK;
    }
    HRESULT unregister_category(Resource resource) override {
        if (resource != Resource::keyboard_category) return E_INVALIDARG;
        category = false; return S_OK;
    }
};
struct FakeMarkers final : OwnerMarkerStore {
    OwnerMarker value;
    std::vector<std::string> calls;
    HRESULT read(Resource, OwnerMarker* out) override { calls.push_back("read"); *out = value; return S_OK; }
    HRESULT write(Resource, const OwnerMarker& marker) override { calls.push_back("write"); value = marker; return S_OK; }
    HRESULT remove(Resource) override { calls.push_back("remove"); value = {}; return S_OK; }
};
struct ProfileBackend final : Backend {
    WindowsTsfSharedStore& store;
    explicit ProfileBackend(WindowsTsfSharedStore& value) : store(value) {}
    HRESULT read(Resource resource, ResourceState* state) override { return store.read(resource, state); }
    HRESULT apply(Resource resource, const ResourceState& state) override { return store.apply(resource, state); }
    HRESULT restore(Resource resource, const ResourceState& state) override { return store.restore(resource, state); }
};
struct RecordingObserver final : TransactionObserver {
    HRESULT last_read = S_OK;
    bool mutation_possible = false;
    HRESULT begin(TransactionOperation, const ResourceStates&) override { return S_OK; }
    HRESULT before(Resource, TransactionMutation, const ResourceState&, const ResourceState&) override { return S_OK; }
    HRESULT after(Resource, TransactionMutation, HRESULT, HRESULT read, const ResourceState*, bool may_mutate) override {
        last_read = read; mutation_possible = may_mutate; return S_OK;
    }
};
struct Attempt {
    HRESULT status = E_UNEXPECTED;
    std::vector<std::string> api_calls, marker_calls;
    bool present = false, enabled = false;
};
Attempt attempt(Failure failure, SharedStoreTrace* trace) {
    FakeProfileApi api(failure);
    FakeMarkers markers;
    WindowsTsfSharedStore store(api, markers, L"trace-test-owner", trace);
    ResourceState before;
    const HRESULT read = store.read(Resource::profile, &before);
    ResourceState desired{true, true, L"trace-test-owner", L"C:\\LiuKai\\branding.dll", L"", {}};
    const HRESULT result = read == S_OK ? store.apply(Resource::profile, desired) : read;
    return {result, api.calls, markers.calls, api.present, api.enabled};
}
const SharedStoreTraceEvent* find(const SharedStoreTrace& trace, SharedStoreTraceStage stage, HRESULT status) {
    for (size_t i = 0; i < trace.size(); ++i)
        if (trace.data()[i].resource == Resource::profile && trace.data()[i].stage == stage &&
            trace.data()[i].status == status) return trace.data() + i;
    return nullptr;
}
std::string trace_json(const SharedStoreTrace& trace) {
    std::ostringstream output;
    auto* previous = std::cout.rdbuf(output.rdbuf());
    diagnostic_trace_json(trace);
    std::cout.rdbuf(previous);
    return output.str();
}
int main() {
    const HRESULT invalid = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    for (Failure failure : {Failure::none, Failure::registration, Failure::enable_ignored,
                            Failure::icon, Failure::user_read}) {
        SharedStoreTrace trace;
        const auto plain = attempt(failure, nullptr);
        const auto traced = attempt(failure, &trace);
        CHECK(plain.status == traced.status && plain.api_calls == traced.api_calls &&
              plain.marker_calls == traced.marker_calls && plain.present == traced.present &&
              plain.enabled == traced.enabled);
        CHECK(find(trace, SharedStoreTraceStage::RegisterProfile,
                   failure == Failure::registration ? invalid : S_OK));
        if (failure == Failure::none) {
            CHECK(traced.status == S_OK && traced.present && traced.enabled);
            CHECK(find(trace, SharedStoreTraceStage::FinalVerification, S_OK));
        } else if (failure == Failure::enable_ignored) {
            CHECK(traced.status == invalid);
            const auto* requested = find(trace, SharedStoreTraceStage::ProfileUserStateRestore, S_OK);
            const auto* observed = find(trace, SharedStoreTraceStage::ProfileUserStateVerification, invalid);
            CHECK(requested && requested->user_state_known && requested->enabled);
            CHECK(observed && observed->user_state_known && !observed->enabled);
            const auto json = trace_json(trace);
            CHECK(json.find("\"stage\":\"profile_user_state_verification\"") != std::string::npos);
            CHECK(json.find("\"hr\":2147942413") != std::string::npos);
            CHECK(json.find("\"user_state_known\":true") != std::string::npos);
            CHECK(json.find("\"enabled\":false") != std::string::npos);
        } else if (failure == Failure::icon) {
            CHECK(traced.status == invalid && find(trace, SharedStoreTraceStage::ProfileIconReadback, invalid));
        } else if (failure == Failure::user_read) {
            CHECK(traced.status == E_ACCESSDENIED);
            const auto* event = find(trace, SharedStoreTraceStage::ProfileUserStateReadback, E_ACCESSDENIED);
            CHECK(event && !event->user_state_known);
        } else CHECK(traced.status == invalid);
        CHECK(trace_json(trace).find("unknown") == std::string::npos);
    }
    SharedStoreTrace full;
    for (int i = 0; i < 256; ++i)
        full.record({SharedStoreTracePhase::Read, SharedStoreTraceStage::ProfileReadback, Resource::profile, S_OK});
    CHECK(attempt(Failure::enable_ignored, &full).status == invalid);
    CHECK(find(full, SharedStoreTraceStage::ProfileUserStateVerification, invalid));
    CHECK(full.dropped_count() > 0);
    CHECK(trace_json(full).find("\"dropped_count\":0") == std::string::npos);
    // RegisterProfile succeeded, but TSF can briefly report the profile as
    // absent. That observation must never turn rollback into a false success.
    FakeProfileApi delayed(Failure::none);
    FakeMarkers delayed_markers;
    WindowsTsfSharedStore delayed_store(delayed, delayed_markers, L"trace-test-owner");
    ResourceState before;
    CHECK(delayed_store.read(Resource::profile, &before) == S_OK && !before.present);
    delayed.hide_created_profile = true;
    ResourceState desired{true, true, L"trace-test-owner", L"C:\\LiuKai\\branding.dll", L"", {}};
    CHECK(delayed_store.apply(Resource::profile, desired) != S_OK);
    CHECK(delayed.present && !delayed_markers.value.present);
    CHECK(delayed_store.restore(Resource::profile, before) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(delayed.present);
    delayed.hide_created_profile = false;
    ResourceState visible;
    CHECK(delayed_store.read(Resource::profile, &visible) == S_OK && visible.present && visible.created_by_lifecycle);
    CHECK(delayed_store.restore(Resource::profile, before) == S_OK);
    CHECK(!delayed.present);
    FakeProfileApi transaction_api(Failure::none);
    FakeMarkers transaction_markers;
    WindowsTsfSharedStore transaction_store(transaction_api, transaction_markers, L"trace-test-owner");
    ProfileBackend backend(transaction_store);
    ResourceStates transaction_before{}, transaction_target{};
    transaction_target[detail::index(Resource::profile)] = desired;
    RecordingObserver observer;
    transaction_api.hide_created_profile = true;
    auto result = detail::apply_transaction(backend, transaction_before, transaction_target,
        {Resource::profile}, &observer);
    CHECK(result.operation != S_OK && result.cleanup == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(observer.last_read == HRESULT_FROM_WIN32(ERROR_RETRY) && observer.mutation_possible);
    CHECK(transaction_api.present && !transaction_markers.value.present);
    FakeProfileApi delayed_category(Failure::none);
    FakeMarkers category_markers;
    WindowsTsfSharedStore category_store(delayed_category, category_markers, L"trace-test-owner");
    ResourceState category_before;
    CHECK(category_store.read(Resource::keyboard_category, &category_before) == S_OK && !category_before.present);
    delayed_category.hide_created_category = true;
    ResourceState category_desired{true, true, L"trace-test-owner", L"", L"", {}};
    CHECK(category_store.apply(Resource::keyboard_category, category_desired) != S_OK);
    CHECK(delayed_category.category);
    CHECK(category_store.restore(Resource::keyboard_category, category_before) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(delayed_category.category);
    delayed_category.hide_created_category = false;
    ResourceState visible_category;
    CHECK(category_store.read(Resource::keyboard_category, &visible_category) == S_OK &&
        visible_category.present && visible_category.created_by_lifecycle);
    CHECK(category_store.restore(Resource::keyboard_category, category_before) == S_OK);
    CHECK(!delayed_category.category);
    // An error return is not proof of no side effect. Never adopt an
    // unmarked profile or report that rollback completed after such a call.
    FakeProfileApi failed_after_effect(Failure::registration_after_effect);
    FakeMarkers failed_markers;
    WindowsTsfSharedStore failed_store(failed_after_effect, failed_markers, L"trace-test-owner");
    ResourceState failed_before;
    CHECK(failed_store.read(Resource::profile, &failed_before) == S_OK);
    CHECK(failed_store.apply(Resource::profile, desired) == invalid);
    CHECK(failed_after_effect.present);
    ResourceState unknown;
    CHECK(failed_store.read(Resource::profile, &unknown) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(failed_store.restore(Resource::profile, failed_before) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(failed_after_effect.present && !failed_markers.value.present);
    // A foreign icon binding must not be adopted as the profile we created.
    FakeProfileApi collision(Failure::none);
    FakeMarkers collision_markers;
    WindowsTsfSharedStore collision_store(collision, collision_markers, L"trace-test-owner");
    ResourceState collision_before;
    CHECK(collision_store.read(Resource::profile, &collision_before) == S_OK);
    collision.hide_created_profile = true;
    CHECK(collision_store.apply(Resource::profile, desired) != S_OK);
    collision.icon = L"C:\\Other\\branding.dll";
    collision.hide_created_profile = false;
    CHECK(collision_store.read(Resource::profile, &unknown) == HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
    CHECK(collision.present);
    FakeProfileApi failed_category(Failure::none);
    FakeMarkers failed_category_markers;
    WindowsTsfSharedStore failed_category_store(failed_category, failed_category_markers, L"trace-test-owner");
    ResourceState failed_category_before;
    CHECK(failed_category_store.read(Resource::keyboard_category, &failed_category_before) == S_OK);
    failed_category.category_fails_after_create = true;
    CHECK(failed_category_store.apply(Resource::keyboard_category, category_desired) == E_ACCESSDENIED);
    CHECK(failed_category.category);
    CHECK(failed_category_store.read(Resource::keyboard_category, &unknown) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(failed_category_store.restore(Resource::keyboard_category, failed_category_before) == HRESULT_FROM_WIN32(ERROR_RETRY));
    CHECK(failed_category.category && !failed_category_markers.value.present);
    std::cout << "profile trace: success and four failure paths, API-call parity, JSON and truncation passed; in-memory dependencies only\n";
}
