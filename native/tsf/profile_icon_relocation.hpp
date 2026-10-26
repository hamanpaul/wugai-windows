#pragma once
#include "profile_display_update.hpp"
#include <filesystem>

namespace boshiamy::profile_icon {
using Snapshot = profile_display::ProfileSnapshot;
class Access {
public:
    virtual ~Access() = default;
    virtual HRESULT read(Snapshot*) = 0;
    // Implementations must preserve the captured registration arguments and
    // must compare the full captured snapshot again immediately before write.
    virtual HRESULT write(const Snapshot&, const std::wstring& icon_path) = 0;
};
struct Result {
    HRESULT operation = E_UNEXPECTED;
    HRESULT cleanup = S_OK;
    bool rollback_attempted = false;
    bool uncertain = false;
};
inline HRESULT conflict() { return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH); }
inline bool usable_path(const std::wstring& path) {
    if (path.empty() || path.size() >= MAX_PATH || path.find(L'\0') != std::wstring::npos) return false;
    try { return std::filesystem::path(path).is_absolute(); } catch (...) { return false; }
}
inline bool registration_shape(const Snapshot& s) {
    return profile_display::detail::usable_expected(s) &&
        (s.api_description == profile_display::kDisplayDescription || s.api_description == L"嘸＋改 Liu-Kai") &&
        s.registry.icon_index == 0 && s.registry.enabled_by_default == 1 &&
        s.registry.hidden_in_settings == 0 && s.registry.subitem_in_settings == 0 &&
        s.substitute_hkl == 0 && s.hkl == 0 && usable_path(s.registry.icon_file);
}
// Only the icon-file field changes. Capabilities/categories remain the separate
// category transaction's responsibility and are never synthesized here.
inline Result relocate(Access& access, const std::wstring& expected_icon,
                       const std::wstring& target_icon) {
    if (!usable_path(expected_icon) || !usable_path(target_icon)) return {E_INVALIDARG};
    Snapshot before, check;
    HRESULT status = access.read(&before);
    if (status != S_OK) return {status};
    status = access.read(&check);
    if (status != S_OK) return {status};
    if (!profile_display::detail::same_snapshot(before, check) ||
        before.registry.icon_file != expected_icon) return {conflict()};
    if (!registration_shape(before)) return {E_INVALIDARG};
    if (expected_icon == target_icon) return {S_OK};
    Snapshot target = before; target.registry.icon_file = target_icon;
    const HRESULT written = access.write(before, target_icon);
    Snapshot after;
    const HRESULT observed = access.read(&after);
    if (observed != S_OK) return {written != S_OK ? written : observed, observed, false, true};
    const bool at_target = profile_display::detail::same_snapshot(after, target);
    if (written == S_OK && at_target) return {S_OK};
    const HRESULT operation = written != S_OK ? written : conflict();
    if (profile_display::detail::same_snapshot(after, before)) return {operation};
    // A complete exact target is attributable. Unknown or partial metadata
    // changes are retained for recovery rather than overwritten by an inverse.
    if (!at_target) return {operation, conflict(), false, true};
    const HRESULT rolled = access.write(after, expected_icon);
    Snapshot restored;
    const HRESULT readback = access.read(&restored);
    if (readback != S_OK) return {operation, readback, true, true};
    if (profile_display::detail::same_snapshot(restored, before)) return {operation, S_OK, true, false};
    return {operation, rolled != S_OK ? rolled : conflict(), true, true};
}
} // namespace boshiamy::profile_icon
