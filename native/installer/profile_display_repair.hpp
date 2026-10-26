#pragma once
#include "profile_display_backend.hpp"
#include "durable_transaction_observer.hpp"
#include "file_deployment.hpp"

namespace boshiamy::installer::profile_display_repair {
using Snapshot = profile_display_state::DisplaySnapshot;
inline HRESULT fingerprint(const Snapshot& value, std::string* out) {
    if (!out) return E_POINTER;
    out->clear();
    std::vector<BYTE> image;
    const HRESULT status = profile_display_state::encode(value, &image);
    if (status != S_OK) return status;
    *out = files::detail::sha256_bytes(std::string(image.begin(), image.end()));
    return files::detail::valid_hash(*out) ? S_OK : E_FAIL;
}

// An explicit metadata-only repair never writes COM registration/categories or
// uses their wider install health as the definition of a successful name repair.
// DisplayBackend supplies the metadata-specific target health above this view.
class RegistrationView final : public Backend {
    Backend& inner_;
public:
    explicit RegistrationView(Backend& inner) : inner_(inner) {}
    HRESULT read(Resource resource, ResourceState* value) override { return inner_.read(resource, value); }
    HRESULT apply(Resource, const ResourceState&) override { return E_ACCESSDENIED; }
    HRESULT restore(Resource, const ResourceState&) override { return E_ACCESSDENIED; }
    HRESULT project_apply_state(Resource resource, const ResourceState& value, ResourceState* out) const override {
        return inner_.project_apply_state(resource, value, out);
    }
};

// Caller must supply a display decorator bound to exact-key authority and a
// validated installed package. expected_display_sha256 is the complete PDS1
// fingerprint from a separately inspectable plan, not a visible-name comparison.
inline LifecycleResult execute(profile_display_backend::DisplayBackend& backend, const InstallRequest& request,
                               const std::string& expected_display_sha256, TransactionObserver& observer) {
    if (request.owner != L"LiuKai-TSF-v1" || request.include_x86 || !request.x86_binary_path.empty() ||
        !request.expected_x86_path.empty() || !files::detail::valid_hash(expected_display_sha256) ||
        !profile_display_plan::detail::valid_stable_module(request.x64_binary_path) ||
        !profile_display_plan::detail::valid_stable_module(request.icon_path) ||
        !profile_display::resource_name::exact_path_equal(request.expected_x64_path, request.x64_binary_path))
        return {E_INVALIDARG, S_OK};
    ResourceStates before{};
    HRESULT status = installer::detail::read_all(backend, before);
    if (status != S_OK) return {status, S_OK};
    const auto& com = before[static_cast<size_t>(Resource::com_x64)];
    const auto& profile = before[static_cast<size_t>(Resource::profile)];
    if (!com.present || !com.created_by_lifecycle || com.owner != request.owner ||
        com.threading_model != L"Apartment" || !profile.present ||
        !profile_display::resource_name::exact_path_equal(com.path, request.x64_binary_path))
        return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    ResourceState legacy;
    Snapshot display;
    status = profile_display_backend::envelope::unpack(profile, &legacy, &display);
    if (status != S_OK) return {status, S_OK};
    std::string actual;
    status = fingerprint(display, &actual);
    if (status != S_OK) return {status, S_OK};
    if (actual != expected_display_sha256) return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), S_OK};
    auto desired = before;
    status = backend.prepare_apply_targets(TransactionOperation::repair_profile_display, request, before, &desired);
    if (status != S_OK) return {status, S_OK};
    // Even a faulty target hook cannot enlarge this operation's mutation set.
    for (size_t i = 0; i < before.size(); ++i)
        if (i != static_cast<size_t>(Resource::profile) && before[i] != desired[i])
            return {E_ACCESSDENIED, S_OK};
    const auto unchanged = [&]() -> HRESULT {
        for (size_t i = 0; i < before.size(); ++i) {
            if (i == static_cast<size_t>(Resource::profile)) continue;
            ResourceState current;
            const HRESULT read = backend.read(static_cast<Resource>(i), &current);
            if (read != S_OK) return read;
            if (current != before[i]) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        }
        return S_OK;
    };
    const auto result = installer::detail::apply_transaction(backend, before, desired, {Resource::profile},
        &observer, TransactionOperation::repair_profile_display, [&](Resource) { return unchanged(); });
    if (result.operation != S_OK || result.cleanup != S_OK) return result;
    ResourceStates final{};
    status = installer::detail::read_all(backend, final);
    if (status != S_OK) return {status, E_ACCESSDENIED};
    for (size_t i = 0; i < before.size(); ++i)
        if (i != static_cast<size_t>(Resource::profile) && final[i] != before[i])
            return {HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), E_ACCESSDENIED};
    status = observer.commit(final);
    return {status, status == S_OK ? S_OK : E_ACCESSDENIED};
}
}
