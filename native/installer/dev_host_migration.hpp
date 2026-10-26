#pragma once

#include "dual_architecture_transaction.hpp"

namespace boshiamy::installer::dev_host_migration {
namespace detail {
inline HRESULT conflict() {
    return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}

inline bool others_equal(const ResourceStates& current, const ResourceStates& expected) {
    for (size_t i = 1; i < current.size(); ++i)
        if (current[i] != expected[i]) return false;
    return true;
}

inline HRESULT observe(TransactionObserver* observer, TransactionMutation mutation,
                       HRESULT operation, HRESULT readback, const ResourceStates* current, bool may_mutate) {
    return observer ? observer->after(Resource::com_x64, mutation, operation, readback,
                                     current ? &(*current)[0] : nullptr, may_mutate) : S_OK;
}

// Compensation requires both a proven complete target and a fresh full CAS.
// Unknown/foreign state is retained for explicit recovery; never touch the other resources.
inline LifecycleResult restore_verified(Backend& backend, const ResourceStates& before,
                                        const ResourceStates& written, TransactionObserver* observer,
                                        HRESULT operation) {
    if (observer) {
        const HRESULT status = observer->before(Resource::com_x64, TransactionMutation::restore,
                                                written[0], before[0]);
        if (status != S_OK) return {operation, status};
    }
    ResourceStates current{};
    HRESULT read = installer::detail::read_all(backend, current);
    if (read != S_OK || current != written) {
        const HRESULT failed = read != S_OK ? read : conflict();
        const HRESULT logged = observe(observer, TransactionMutation::restore, failed, read,
                                       read == S_OK ? &current : nullptr, false);
        return {operation, logged != S_OK ? logged : failed};
    }
    const HRESULT restored = backend.restore(Resource::com_x64, before[0]);
    const bool may_mutate = SUCCEEDED(restored) || backend.failure_may_have_mutated(Resource::com_x64, restored);
    ResourceStates after{};
    read = installer::detail::read_all(backend, after);
    const HRESULT cleanup = restored != S_OK ? restored : read != S_OK ? read :
        after != before ? conflict() : S_OK;
    const HRESULT logged = observe(observer, TransactionMutation::restore, cleanup, read,
                                   read == S_OK ? &after : nullptr, may_mutate);
    return {operation, logged != S_OK ? logged : cleanup};
}
} // namespace detail

// Registration-only stage. The caller publishes/validates the managed package,
// supplies the separately captured full prestate, and owns final durable commit.
// No shared/profile/COM32 mutation, profile ownership adoption, or tray health runs here.
inline LifecycleResult execute(Backend& backend, const InstallRequest& request,
                               const ResourceStates& expected_before, TransactionObserver* observer = nullptr) {
    if (request.include_machine_com_x64 || request.include_x86 || !request.x86_binary_path.empty() || !request.expected_x86_path.empty() ||
        request.owner.empty() || !installer::detail::absolute_path(request.expected_x64_path) ||
        !installer::detail::absolute_path(request.x64_binary_path) ||
        installer::detail::same_path(request.expected_x64_path, request.x64_binary_path))
        return {E_INVALIDARG, S_OK};
    if (expected_before[static_cast<size_t>(Resource::machine_com_x64)].present)
        return {HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), S_OK};
    const auto& original = expected_before[0];
    if (!original.present || !original.created_by_lifecycle || original.owner != request.owner ||
        original.threading_model != L"Apartment" ||
        !installer::detail::same_path(original.path, request.expected_x64_path) || !expected_before[2].present)
        return {detail::conflict(), S_OK};
    ResourceStates current{};
    HRESULT status = installer::detail::read_all(backend, current);
    if (status != S_OK) return {status, status};
    if (current != expected_before) return {detail::conflict(), detail::conflict()};
    ResourceState target = original;
    target.path = request.x64_binary_path;
    if (observer) {
        status = observer->begin(TransactionOperation::migrate_x64_dev_host, expected_before);
        if (status != S_OK) return {status, S_OK};
        status = observer->before(Resource::com_x64, TransactionMutation::apply, original, target);
        if (status != S_OK) return {status, S_OK};
    }
    // This full guard follows before(), so observer callbacks cannot bypass it.
    status = installer::detail::read_all(backend, current);
    if (status != S_OK || current != expected_before) {
        const HRESULT failed = status != S_OK ? status : detail::conflict();
        const HRESULT logged = detail::observe(observer, TransactionMutation::apply, failed, status,
                                              status == S_OK ? &current : nullptr, false);
        return {failed, logged != S_OK ? logged : failed};
    }
    const HRESULT applied = backend.apply(Resource::com_x64, target);
    const bool may_mutate = SUCCEEDED(applied) || backend.failure_may_have_mutated(Resource::com_x64, applied);
    ResourceStates after{};
    const HRESULT read = installer::detail::read_all(backend, after);
    const bool unchanged_others = read == S_OK && detail::others_equal(after, expected_before);
    const HRESULT verified = read == S_OK ? backend.verify_applied(Resource::com_x64, target, after[0]) : S_OK;
    const HRESULT operation = applied != S_OK ? applied : read != S_OK ? read :
        !unchanged_others ? detail::conflict() : verified;
    const HRESULT logged = detail::observe(observer, TransactionMutation::apply, operation, read,
                                          read == S_OK ? &after : nullptr, may_mutate);
    // A failed observation may leave a durable intent unresolved. Do not write again.
    if (logged != S_OK) return {operation != S_OK ? operation : logged, logged};
    if (read != S_OK) return {operation, read};
    if (!unchanged_others) return {operation, detail::conflict()};
    if (operation != S_OK) {
        if (after[0] == original) return {operation, S_OK};
        if (!may_mutate) return {operation, detail::conflict()};
        // A partial/foreign opaque tree is not ours merely because an API failed.
        if (verified != S_OK) return {operation, verified};
        return detail::restore_verified(backend, expected_before, after, observer, operation);
    }
    // The coordinator owns commit, but this stage must return a fully guarded state.
    ResourceStates final{};
    status = installer::detail::read_all(backend, final);
    if (status != S_OK) return {status, status};
    if (!detail::others_equal(final, expected_before) || final[0] != after[0])
        return {detail::conflict(), detail::conflict()};
    status = backend.verify_applied(Resource::com_x64, target, final[0]);
    return {status, status == S_OK ? S_OK : status};
}
} // namespace boshiamy::installer::dev_host_migration
