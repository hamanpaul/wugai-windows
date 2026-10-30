#include "../installer/durable_compensation_marker.hpp"
#include "../installer/tsf_shared_store.hpp"
#include <iostream>

namespace installer = boshiamy::installer;
namespace journal = installer::durable_journal;
namespace observer = installer::durable_observer;
namespace compensation = installer::durable_compensation_marker;
#define CHECK(value) do { if (!(value)) { std::cerr << "check failed at " << __LINE__ << ": " #value "\n"; return false; } } while (false)

// Only exercise the production projection. No Windows registration or registry
// adapter is instantiated, and any accidental API/store access fails the test.
struct UnusedApi final : installer::TsfSharedApi {
    unsigned calls = 0;
    HRESULT profile_present(bool*) override { ++calls; return E_UNEXPECTED; }
    HRESULT register_profile(const std::wstring&, bool) override { ++calls; return E_UNEXPECTED; }
    HRESULT unregister_profile() override { ++calls; return E_UNEXPECTED; }
    HRESULT profile_user_state(bool*, bool*) override { ++calls; return E_UNEXPECTED; }
    HRESULT restore_profile_user_state(bool, bool) override { ++calls; return E_UNEXPECTED; }
    HRESULT category_present(installer::Resource, bool*) override { ++calls; return E_UNEXPECTED; }
    HRESULT register_category(installer::Resource) override { ++calls; return E_UNEXPECTED; }
    HRESULT unregister_category(installer::Resource) override { ++calls; return E_UNEXPECTED; }
};
struct UnusedMarkers final : installer::OwnerMarkerStore {
    unsigned calls = 0;
    HRESULT read(installer::Resource, installer::OwnerMarker*) override { ++calls; return E_UNEXPECTED; }
    HRESULT write(installer::Resource, const installer::OwnerMarker&) override { ++calls; return E_UNEXPECTED; }
    HRESULT remove(installer::Resource) override { ++calls; return E_UNEXPECTED; }
};

static journal::Identity identity() {
    journal::Identity id;
    id.schema_version = 3;
    id.owner = "LiuKai-TSF-v1";
    id.token = "compensation-projection";
    id.install_root = L"C:\\fixture\\install";
    id.storage_root = L"C:\\fixture\\.liukai-installer-ledger-machine-com-x64\\LiuKai-TSF-v1\\compensation-projection";
    id.package_version = "fixture";
    id.package_manifest_sha256.assign(64, '1');
    return id;
}

static installer::ResourceState installed_state(installer::Resource resource, bool previous = false) {
    installer::ResourceState state;
    state.present = state.created_by_lifecycle = true;
    state.owner = L"LiuKai-TSF-v1";
    if (resource == installer::Resource::profile)
        state.path = previous ? L"C:\\fixture\\old-branding.dll" : L"C:\\fixture\\branding.dll";
    installer::OwnerMarker marker;
    marker.present = true; marker.owner = state.owner; marker.path = state.path;
    installer::encode_owner_marker(marker, &state.private_data);
    if (resource == installer::Resource::profile) {
        const auto* magic = reinterpret_cast<const unsigned char*>(&installer::kProfileUserStateMagic);
        state.private_data.insert(state.private_data.end(), magic, magic + sizeof(uint32_t));
        state.private_data.push_back(previous ? 0 : 1);
        state.private_data.push_back(previous ? 1 : 0);
    }
    return state;
}

static void append_pair(journal::Document* document, installer::Resource resource,
        const char* action, const std::string& before, const std::string& desired,
        HRESULT status, const std::string& observed, bool may_mutate) {
    journal::Event intent, observation;
    intent.kind = journal::Event::Kind::intent;
    intent.intent.sequence = document->events.size() / 2 + 1;
    intent.intent.resource = observer::detail::resource_name(resource);
    intent.intent.action = action;
    intent.intent.expected_before_fingerprint = before;
    intent.intent.desired_fingerprint = desired;
    observation.kind = journal::Event::Kind::observation;
    observation.observation.sequence = intent.intent.sequence;
    observation.observation.operation_status = status;
    observation.observation.observed_fingerprint = observed;
    observation.observation.mutation_may_have_occurred = may_mutate;
    document->events.push_back(intent); document->events.push_back(observation);
}

static bool test_shared_projection(installer::Resource resource, bool present_before = false) {
    UnusedApi api; UnusedMarkers markers;
    installer::WindowsTsfSharedStore store(api, markers, L"LiuKai-TSF-v1");
    const compensation::ApplyProjection projection = [&](installer::Resource r,
            const installer::ResourceState& state, installer::ResourceState* out) {
        return store.project_apply_state(r, state, out);
    };
    const auto id = identity();
    installer::ResourceStates prestate{};
    const auto original = present_before ? installed_state(resource, true) : installer::ResourceState{};
    prestate[static_cast<size_t>(resource)] = original;
    const auto written = installed_state(resource);
    installer::ResourceState projected_absent, projected_written;
    CHECK(projection(resource, original, &projected_absent) == S_OK);
    CHECK(projection(resource, written, &projected_written) == S_OK);
    std::string absent_raw, absent_apply, written_raw, written_apply;
    CHECK(observer::detail::fingerprint_state(id, resource, original, &absent_raw));
    CHECK(observer::detail::fingerprint_state(id, resource, projected_absent, &absent_apply, false));
    CHECK(observer::detail::fingerprint_state(id, resource, written, &written_raw));
    CHECK(observer::detail::fingerprint_state(id, resource, projected_written, &written_apply, false));
    CHECK(absent_raw != absent_apply && written_raw != written_apply);
    const auto valid = [&](const journal::Document& document, const compensation::ApplyProjection& projector) {
        return compensation::resource_compensation_valid(id, document, prestate, 0, document.events.size(), projector);
    };
    const HRESULT invalid_data = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

    // Failed-without-change needs no restore, for both absence and an existing
    // profile with complete marker/LSU1 state. Absent projections also have a tag.
    journal::Document unchanged;
    append_pair(&unchanged, resource, "apply", absent_raw, written_apply, invalid_data, absent_apply, true);
    CHECK(valid(unchanged, projection));
    CHECK(!valid(unchanged, {}));

    // A partial write followed by exact restoration crosses projection domains.
    journal::Document restored;
    append_pair(&restored, resource, "apply", absent_raw, written_apply, invalid_data, written_apply, true);
    CHECK(!valid(restored, projection)); // missing restore
    append_pair(&restored, resource, "restore", written_raw, absent_raw, S_OK, absent_raw, true);
    CHECK(valid(restored, projection));
    CHECK(!valid(restored, {}));
    auto altered = restored;
    altered.events[3].observation.observed_fingerprint = written_raw;
    CHECK(!valid(altered, projection)); // restored state drift
    altered = restored; altered.events[2].intent.desired_fingerprint = written_raw;
    CHECK(!valid(altered, projection)); // wrong restore target
    altered = restored; altered.events[2].intent.expected_before_fingerprint = "invalid";
    CHECK(!valid(altered, projection));
    altered = restored; altered.events[3].observation.operation_status = E_ACCESSDENIED;
    CHECK(!valid(altered, projection));
    altered = restored; altered.events[1].observation.observed_fingerprint = observer::detail::unknown_fingerprint();
    CHECK(!valid(altered, projection));
    altered = restored; altered.events[1].observation.mutation_may_have_occurred = false;
    CHECK(!valid(altered, projection)); // failure claiming no mutation despite a changed readback
    altered = restored; altered.events[0].intent.expected_before_fingerprint = written_raw;
    CHECK(!valid(altered, projection)); // prestate mismatch
    altered = restored; altered.events[1].observation.operation_status = S_OK;
    CHECK(valid(altered, projection)); // successful apply rolled back for a later failure
    altered.events[1].observation.observed_fingerprint = absent_apply;
    CHECK(!valid(altered, projection)); // successful apply did not reach its desired state

    // Legacy and identity-projection journals stay in one domain and must
    // retain the exact link from apply readback to restore expected-before.
    journal::Document legacy;
    append_pair(&legacy, resource, "apply", absent_raw, written_raw, invalid_data, written_raw, true);
    append_pair(&legacy, resource, "restore", written_raw, absent_raw, S_OK, absent_raw, true);
    const compensation::ApplyProjection unchanged_projection = [](installer::Resource,
            const installer::ResourceState& state, installer::ResourceState* out) { *out = state; return S_OK; };
    CHECK(valid(legacy, {}) && valid(legacy, unchanged_projection));
    legacy.events[2].intent.expected_before_fingerprint = absent_raw;
    CHECK(!valid(legacy, {}) && !valid(legacy, unchanged_projection));
    CHECK(api.calls == 0 && markers.calls == 0);
    return true;
}

int main() {
    for (const auto resource : {installer::Resource::profile, installer::Resource::keyboard_category,
            installer::Resource::system_tray_category, installer::Resource::ui_element_category})
        if (!test_shared_projection(resource)) return 1;
    if (!test_shared_projection(installer::Resource::profile, true)) return 1;
    std::cout << "shared apply/restore compensation projection assertions passed\n";
    return 0;
}
