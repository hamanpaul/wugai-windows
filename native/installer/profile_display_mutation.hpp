#pragma once
#include "profile_display_snapshot.hpp"

namespace boshiamy::installer::profile_display_mutation {
using Snapshot = profile_display_state::DisplaySnapshot;
using RawValue = profile_display_state::resource_name::RawRegistryValue;
enum class Field { description64, description32, binding64, binding32 };

class RawAccess {
public:
    virtual ~RawAccess() = default;
    virtual HRESULT capture(Snapshot*) = 0;
    virtual HRESULT authorize(const Snapshot& expected, const Snapshot& target) = 0;
    virtual HRESULT conditional_write(Field, const RawValue& expected, const RawValue& target) = 0;
    // Bind this to the documented key scope; equal captured values alone do
    // not prove aliasing. HKLM SOFTWARE\Microsoft\CTF\TIP is a shared key.
    virtual bool shared_views() const { return false; }
};

namespace detail {
inline const RawValue& value(const Snapshot& snapshot, Field field) {
    if (field == Field::description64) return snapshot.descriptions.registry64;
    if (field == Field::description32) return snapshot.descriptions.registry32;
    if (field == Field::binding64) return snapshot.display_binding.registry64;
    return snapshot.display_binding.registry32;
}
inline RawValue& value(Snapshot& snapshot, Field field) {
    return const_cast<RawValue&>(value(static_cast<const Snapshot&>(snapshot), field));
}
inline bool valid_raw(const RawValue& raw) {
    return raw.status == ERROR_FILE_NOT_FOUND
        ? raw.type == 0 && raw.bytes.empty()
        : raw.status == ERROR_SUCCESS && raw.bytes.size() <= 65536;
}
inline bool phase_matches(const Snapshot& observed, const Snapshot& phase,
                          const Snapshot& before, const Snapshot& target) {
    return profile_display::detail::same_except_descriptions(observed.profile, before.profile) &&
        profile_display_state::resource_name::same_binding(observed.descriptions, phase.descriptions) &&
        profile_display_state::resource_name::same_binding(observed.display_binding, phase.display_binding) &&
        (observed.profile.api_description == before.profile.api_description ||
         observed.profile.api_description == target.profile.api_description) &&
        (observed.profile.registry.description == before.profile.registry.description ||
         observed.profile.registry.description == target.profile.registry.description);
}
}

// Caller serializes cooperating writers and authorizes the exact hive/view.
// Each value uses compare-before-write/readback, not an atomic Windows CAS.
// This operation never compensates an unknown or foreign state automatically;
// the outer transaction retains its complete snapshots and durable intent.
inline HRESULT exchange(RawAccess& access, const Snapshot& expected, const Snapshot& target) {
    constexpr Field order[] = {Field::description64, Field::description32, Field::binding64, Field::binding32};
    const bool shared = access.shared_views();
    if (!profile_display::detail::same_except_descriptions(expected.profile, target.profile)) return E_INVALIDARG;
    for (Field field : order)
        if (!detail::valid_raw(detail::value(expected, field)) || !detail::valid_raw(detail::value(target, field)))
            return E_INVALIDARG;
    if (shared &&
        (!profile_display_state::resource_name::same_value(expected.descriptions.registry64, expected.descriptions.registry32) ||
         !profile_display_state::resource_name::same_value(expected.display_binding.registry64, expected.display_binding.registry32) ||
         !profile_display_state::resource_name::same_value(target.descriptions.registry64, target.descriptions.registry32) ||
         !profile_display_state::resource_name::same_value(target.display_binding.registry64, target.display_binding.registry32)))
        return E_INVALIDARG;
    try {
        Snapshot current;
        HRESULT status = access.capture(&current);
        if (status != S_OK) return status;
        if (!profile_display_state::equal(current, expected)) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        // Prove both directions before the first write. The permission check
        // describes endpoints; it must not assume the inverse is already live.
        status = access.authorize(expected, target);
        if (status != S_OK) return status;
        status = access.authorize(target, expected);
        if (status != S_OK) return status;
        Snapshot phase = expected;
        bool attempted = false;
        for (Field field : order) {
            if (shared && (field == Field::description32 || field == Field::binding32)) continue;
            status = access.capture(&current);
            if (status != S_OK) return status;
            if ((!attempted && !profile_display_state::equal(current, expected)) ||
                !detail::phase_matches(current, phase, expected, target))
                return HRESULT_FROM_WIN32(attempted ? ERROR_INVALID_DATA : ERROR_REVISION_MISMATCH);
            if (profile_display_state::resource_name::same_value(detail::value(phase, field), detail::value(target, field)))
                continue;
            status = access.authorize(expected, target);
            if (status != S_OK) return status;
            attempted = true;
            const HRESULT written = access.conditional_write(field, detail::value(phase, field), detail::value(target, field));
            const HRESULT readback = access.capture(&current);
            // Preserve even a positive unexpected operation result. A failed
            // getter does not turn that result into success or permit more writes.
            if (written != S_OK) return written;
            if (readback != S_OK) return readback;
            detail::value(phase, field) = detail::value(target, field);
            if (shared && field == Field::description64) phase.descriptions.registry32 = target.descriptions.registry32;
            if (shared && field == Field::binding64) phase.display_binding.registry32 = target.display_binding.registry32;
            if (!detail::phase_matches(current, phase, expected, target))
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        status = access.capture(&current);
        if (status != S_OK) return status;
        return profile_display_state::equal(current, target) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
}
