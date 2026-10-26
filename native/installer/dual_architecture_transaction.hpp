#ifndef LIUKAI_INSTALLER_DUAL_ARCHITECTURE_TRANSACTION_HPP
#define LIUKAI_INSTALLER_DUAL_ARCHITECTURE_TRANSACTION_HPP

#include <windows.h>
#include <array>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace boshiamy::installer {

// The profile and keyboard category are shared by x86 and x64 TSF clients.
// COM registrations are separate registry views and must be handled separately.
enum class Resource : size_t { com_x64 = 0, com_x86 = 1, profile = 2, keyboard_category = 3, system_tray_category = 4, ui_element_category = 5, machine_com_x64 = 6, count = 7 };

struct ResourceState {
    bool present = false;
    bool created_by_lifecycle = false;
    std::wstring owner;
    std::wstring path;
    std::wstring threading_model;
    // Backend-specific values let snapshot/restore preserve custom metadata.
    std::vector<unsigned char> private_data;
    bool operator==(const ResourceState& other) const {
        return present == other.present && created_by_lifecycle == other.created_by_lifecycle &&
               owner == other.owner && path == other.path && threading_model == other.threading_model &&
               private_data == other.private_data;
    }
    bool operator!=(const ResourceState& other) const { return !(*this == other); }
};

struct InstallRequest;
enum class TransactionOperation;
using ResourceStates = std::array<ResourceState, static_cast<size_t>(Resource::count)>;

class Backend {
public:
    virtual ~Backend() = default;
    // read/apply/restore must operate on the exact architecture/shared resource.
    // apply must preserve backend metadata absent from ResourceState; restore must
    // reinstate the complete snapshot, including absence and opaque values.
    virtual HRESULT read(Resource resource, ResourceState* state) = 0;
    virtual HRESULT apply(Resource resource, const ResourceState& desired) = 0;
    virtual HRESULT restore(Resource resource, const ResourceState& snapshot) = 0;
    // Application verifies only the backend-owned semantic fields by default.
    // Backends with opaque rollback bytes may validate a resource-specific
    // post-write projection while retaining exact bytes for restore/CAS.
    virtual HRESULT verify_applied(Resource, const ResourceState& desired, const ResourceState& observed) const {
        return desired == observed ? S_OK : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    }
    // Read-only validation of platform state that is not represented in the
    // reversible resource snapshot (for example, a TSF profile capability).
    // Read-only apply journal projection. Full snapshots/CAS/restore remain exact.
    virtual HRESULT project_apply_state(Resource, const ResourceState& state, ResourceState* out) const {
        if (!out) return E_POINTER;
        *out = state;
        return S_OK;
    }
    virtual HRESULT verify_install_health() { return S_OK; }
    virtual HRESULT validate_install(const InstallRequest&) { return S_OK; }
    virtual HRESULT prepare_apply_targets(TransactionOperation, const InstallRequest&,
        const ResourceStates&, ResourceStates* desired) { return desired ? S_OK : E_POINTER; }

    virtual bool failure_may_have_mutated(Resource, HRESULT) const { return true; }
};

struct InstallRequest {
    std::wstring owner;
    std::wstring x64_binary_path;
    std::wstring x86_binary_path;
    std::wstring icon_path;
    std::wstring expected_x86_path;
    std::wstring expected_x64_path;
    bool include_x86 = true;
    // Explicit, x64-only paired HKCU64/HKLM64 lifecycle policy.
    bool include_machine_com_x64 = false;
};

struct LifecycleResult { HRESULT operation = S_OK; HRESULT cleanup = S_OK; };

enum class TransactionOperation { install_or_repair, uninstall, repair_profile_display, migrate_x64_dev_host };
enum class TransactionMutation { apply, restore };


// Optional durable observer. A successful before() is required before each
// mutation; after() receives an independent backend readback of the result.
// The observer must reject further mutations while it has an unclosed intent.
class TransactionObserver {
public:
    virtual ~TransactionObserver() = default;
    virtual HRESULT begin(TransactionOperation operation, const ResourceStates& pre_state) = 0;
    virtual HRESULT before(Resource resource, TransactionMutation mutation,
                           const ResourceState& expected, const ResourceState& desired) = 0;
    virtual HRESULT after(Resource resource, TransactionMutation mutation,
                          HRESULT operation_status, HRESULT readback_status,
                          const ResourceState* observed, bool mutation_may_have_occurred) = 0;
    // Coordinator-only terminal hook. Legacy observers remain source-compatible;
    // durable observers override this to append a commit after final readback.
    virtual HRESULT commit(const ResourceStates&) { return S_OK; }
};

namespace detail {
constexpr size_t index(Resource resource) { return static_cast<size_t>(resource); }
inline bool same_path(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}
inline bool absolute_path(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return false;
    try { return std::filesystem::path(path).is_absolute(); } catch (...) { return false; }
}
inline HRESULT ownership_status(const ResourceState& state, const std::wstring& owner,
                                const std::wstring& expected_path, const std::wstring& desired_path) {
    if (!state.present) return S_OK;
    if (state.owner != owner) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (!expected_path.empty() && !same_path(state.path, expected_path) && !same_path(state.path, desired_path))
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (state.threading_model != L"Apartment")
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    return S_OK;
}
// Machine scope never adopts a COM registration on an owner label alone.
// The platform backend additionally validates its account/install-root binding.
inline HRESULT validate_machine_com_policy(const InstallRequest& request) {
    if (!request.include_machine_com_x64) return S_OK;
    if (request.include_x86 || (!request.expected_x64_path.empty() &&
        !absolute_path(request.expected_x64_path))) return E_INVALIDARG;
    return S_OK;
}
inline HRESULT validate_machine_com_prestate(const InstallRequest& request,
                                             const ResourceStates& before) {
    const auto& machine = before[index(Resource::machine_com_x64)];
    if (!request.include_machine_com_x64)
        return machine.present ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : S_OK;
    for (Resource resource : {Resource::com_x64, Resource::machine_com_x64}) {
        const auto& current = before[index(resource)];
        if (!current.present) continue;
        if (!current.created_by_lifecycle || current.owner != request.owner ||
            current.threading_model != L"Apartment" ||
            (!same_path(current.path, request.x64_binary_path) &&
             (request.expected_x64_path.empty() || !same_path(current.path, request.expected_x64_path))))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    return S_OK;
}
inline HRESULT rollback(Backend& backend, const std::array<ResourceState, index(Resource::count)>& before,
                        const std::vector<Resource>& touched, TransactionObserver* observer=nullptr,
                        std::array<ResourceState,index(Resource::count)>* current=nullptr) {
    HRESULT cleanup = S_OK;
    for (auto it = touched.rbegin(); it != touched.rend(); ++it) {
        const Resource resource=*it;
        if(!observer) {
            HRESULT status=backend.restore(resource,before[index(resource)]);
            if(status!=S_OK&&cleanup==S_OK)cleanup=status;
            continue;
        }
        if(!current) { if(SUCCEEDED(cleanup))cleanup=E_UNEXPECTED;continue; }
        const ResourceState expected=(*current)[index(resource)];
        const ResourceState& target=before[index(resource)];
        if(expected==target)continue;
        HRESULT status=observer->before(resource,TransactionMutation::restore,expected,target);
        if(status!=S_OK){if(cleanup==S_OK)cleanup=status;break;}
        ResourceState observed{};
        HRESULT read_status=backend.read(resource,&observed);
        if(read_status!=S_OK) {
            HRESULT logged=observer->after(resource,TransactionMutation::restore,read_status,read_status,nullptr,false);
            if(cleanup==S_OK)cleanup=logged!=S_OK?logged:read_status;
            if(logged!=S_OK)break;
            continue;
        }
        if(observed!=expected) {
            const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            HRESULT logged=observer->after(resource,TransactionMutation::restore,conflict,S_OK,&observed,false);
            if(cleanup==S_OK)cleanup=logged!=S_OK?logged:conflict;
            continue;
        }
        status=backend.restore(resource,target);
        ResourceState after_state{};
        read_status=backend.read(resource,&after_state);
        const bool may_mutate=SUCCEEDED(status)||backend.failure_may_have_mutated(resource,status);
        HRESULT logged=observer->after(resource,TransactionMutation::restore,status,read_status,
            read_status==S_OK?&after_state:nullptr,may_mutate);
        if(read_status==S_OK)(*current)[index(resource)]=after_state;
        if(logged!=S_OK){if(cleanup==S_OK)cleanup=logged;break;}
        if(read_status!=S_OK){if(cleanup==S_OK)cleanup=read_status;break;}
        // An API may report failure after completing the restore. Exact
        // readback is sufficient to prove cleanup for this resource.
        if(after_state!=target&&cleanup==S_OK)cleanup=status!=S_OK?status:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        else if(status!=S_OK&&SUCCEEDED(status)&&cleanup==S_OK)cleanup=status;
    }
    return cleanup;
}
inline LifecycleResult apply_transaction(Backend& backend,
        const std::array<ResourceState, index(Resource::count)>& before,
        const std::array<ResourceState, index(Resource::count)>& desired,
        const std::vector<Resource>& order, TransactionObserver* observer=nullptr,
        TransactionOperation operation=TransactionOperation::install_or_repair,
        const std::function<HRESULT(Resource)>& mutation_guard={}) {
    if(observer) {
        const HRESULT begin_status=observer->begin(operation,before);
        if(begin_status!=S_OK)return {begin_status,S_OK};
    }
    std::vector<Resource> touched;
    auto current=before;
    HRESULT operation_status=S_OK;
    HRESULT pre_cleanup=S_OK;
    for (Resource resource : order) {
        if (current[index(resource)] == desired[index(resource)]) continue;
        if(observer) {
            const ResourceState expected=current[index(resource)];
            HRESULT status=observer->before(resource,TransactionMutation::apply,expected,desired[index(resource)]);
            if(status!=S_OK){operation_status=status;break;}
            ResourceState observed{};
            HRESULT read_status=backend.read(resource,&observed);
            if(read_status!=S_OK) {
                HRESULT logged=observer->after(resource,TransactionMutation::apply,read_status,read_status,nullptr,false);
                if(logged!=S_OK)return {read_status,logged};
                operation_status=read_status;
                pre_cleanup=read_status;
                break;
            }
            if(observed!=expected) {
                const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                HRESULT logged=observer->after(resource,TransactionMutation::apply,conflict,S_OK,&observed,false);
                if(logged!=S_OK)return {conflict,logged};
                operation_status=conflict;
                current[index(resource)]=observed;
                break;
            }
            if(mutation_guard) {
                const HRESULT guard_status=mutation_guard(resource);
                if(guard_status!=S_OK) {
                    const HRESULT logged=observer->after(resource,TransactionMutation::apply,
                        guard_status,S_OK,&observed,false);
                    if(logged!=S_OK)return {guard_status,logged};
                    operation_status=guard_status;
                    break;
                }
            }
            status=backend.apply(resource,desired[index(resource)]);
            ResourceState after_state{};
            read_status=backend.read(resource,&after_state);
            const bool may_mutate=SUCCEEDED(status)||backend.failure_may_have_mutated(resource,status);
            const HRESULT verify_status=status==S_OK&&read_status==S_OK
                ?backend.verify_applied(resource,desired[index(resource)],after_state):S_OK;
            const HRESULT logged_operation_status=status!=S_OK?status:read_status!=S_OK?read_status:verify_status;
            HRESULT logged=observer->after(resource,TransactionMutation::apply,logged_operation_status,read_status,
                read_status==S_OK?&after_state:nullptr,may_mutate);
            if(read_status==S_OK)current[index(resource)]=after_state;
            if(logged!=S_OK) {
                operation_status=status!=S_OK?status:logged;
                // A post-event failure leaves the durable intent unresolved.
                // Do not issue any further mutation (including rollback).
                return {operation_status,logged};
            }
            if(read_status!=S_OK) {
                operation_status=status!=S_OK?status:read_status;
                pre_cleanup=read_status;
                break; // The state is unknown, so do not restore this resource.
            }
            if(status!=S_OK) {
                operation_status=status;
                if(may_mutate&&after_state!=expected)touched.push_back(resource);
                else if(!may_mutate&&after_state!=expected)
                    pre_cleanup=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                break;
            }
            if(FAILED(verify_status)) {
                operation_status=verify_status;
                pre_cleanup=operation_status;
                break; // Preserve a state that does not match our requested write.
            }
            touched.push_back(resource);
            continue;
        }
        // Include the failing resource in rollback: a platform adapter may have
        // made a partial write before returning its error.
        touched.push_back(resource);
        if(mutation_guard) {
            const HRESULT guard_status=mutation_guard(resource);
            if(guard_status!=S_OK) {
                touched.pop_back();
                return {guard_status,rollback(backend,before,touched)};
            }
        }
        HRESULT status = backend.apply(resource, desired[index(resource)]);
        if (status != S_OK) {
            if (!backend.failure_may_have_mutated(resource, status)) touched.pop_back();
            return {status, rollback(backend, before, touched)};
        }
    }
    bool health_check_failed=false;
    if(operation_status==S_OK && (operation==TransactionOperation::install_or_repair ||
                               operation==TransactionOperation::repair_profile_display)) {
        operation_status=backend.verify_install_health();
        health_check_failed=operation_status!=S_OK;
    }
    if(observer) {
        if(operation_status!=S_OK) {
            const HRESULT rollback_status=rollback(backend,before,touched,observer,&current);
            HRESULT cleanup=pre_cleanup!=S_OK?pre_cleanup:rollback_status;
            if(rollback_status!=S_OK&&cleanup==S_OK)cleanup=rollback_status;
            return {operation_status,cleanup};
        }
    }
    if(FAILED(operation_status)||health_check_failed)
        return {operation_status,rollback(backend,before,touched)};
    return {S_OK, S_OK};
}
inline HRESULT read_all(Backend& backend, std::array<ResourceState, index(Resource::count)>& states) {
    for (size_t i = 0; i < states.size(); ++i) {
        states[i]=ResourceState{};
        HRESULT status = backend.read(static_cast<Resource>(i), &states[i]);
        if (status != S_OK) return status;
    }
    return S_OK;
}
}

inline LifecycleResult install_or_repair(Backend& backend, const InstallRequest& request,
                                         TransactionObserver* observer=nullptr) {
    if (request.owner.empty() || !detail::absolute_path(request.x64_binary_path) ||
        (request.include_x86 && !detail::absolute_path(request.x86_binary_path)) || !detail::absolute_path(request.icon_path)) return {E_INVALIDARG, S_OK};
    const HRESULT policy_status = detail::validate_machine_com_policy(request);
    if (policy_status != S_OK) return {policy_status, S_OK};
    HRESULT package_status = backend.validate_install(request);
    if (FAILED(package_status)) return {package_status, S_OK};
    std::array<ResourceState, detail::index(Resource::count)> before{};
    HRESULT status = detail::read_all(backend, before);
    if (status != S_OK) return {status, S_OK};
    status = detail::validate_machine_com_prestate(request, before);
    if (status != S_OK) return {status, S_OK};
    auto desired = before;
    const Resource coms[] = {Resource::com_x64, Resource::com_x86};
    const std::wstring binaries[] = {request.x64_binary_path, request.x86_binary_path};
    const std::wstring expected[] = {request.expected_x64_path, request.expected_x86_path};
    for (size_t i = 0; i < (request.include_x86 ? 2u : 1u); ++i) {
        Resource resource = coms[i];
        ResourceState& target = desired[detail::index(resource)];
        status = detail::ownership_status(before[detail::index(resource)], request.owner, expected[i], binaries[i]);
        if (FAILED(status)) return {status, S_OK};
        target.present = true;
        target.owner = request.owner;
        target.path = binaries[i];
        target.threading_model = L"Apartment";
        if (!before[detail::index(resource)].present) target.created_by_lifecycle = true;
    }
    if (request.include_machine_com_x64) {
        auto& target = desired[detail::index(Resource::machine_com_x64)];
        target.present = true;
        target.owner = request.owner;
        target.path = request.x64_binary_path;
        target.threading_model = L"Apartment";
        target.created_by_lifecycle = true;
    }
    for (Resource resource : {Resource::profile, Resource::keyboard_category, Resource::system_tray_category, Resource::ui_element_category}) {
        const ResourceState& current = before[detail::index(resource)];
        if (current.present && current.owner != request.owner) return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), S_OK};
    }
    ResourceState& profile = desired[detail::index(Resource::profile)];
    if (!profile.present) {
        profile.present = true; profile.owner = request.owner; profile.created_by_lifecycle = true;
        profile.path = request.icon_path;
    } else if (profile.created_by_lifecycle) {
        // Only metadata created and tracked by this installer can be updated
        // safely. A borrowed legacy profile may have settings the backend
        // cannot enumerate, so leave it untouched.
        profile.path = request.icon_path;
    }
    ResourceState& category = desired[detail::index(Resource::keyboard_category)];
    if (!category.present) { category.present = true; category.owner = request.owner; category.created_by_lifecycle = true; }
    ResourceState& tray_category = desired[detail::index(Resource::system_tray_category)];
    if (!tray_category.present) { tray_category.present = true; tray_category.owner = request.owner; tray_category.created_by_lifecycle = true; }
    ResourceState& ui_category = desired[detail::index(Resource::ui_element_category)];
    if (!ui_category.present) { ui_category.present = true; ui_category.owner = request.owner; ui_category.created_by_lifecycle = true; }
    status=backend.prepare_apply_targets(TransactionOperation::install_or_repair,request,before,&desired);
    if(status!=S_OK)return {status,S_OK};
    std::vector<Resource> order{Resource::com_x64};
    if (request.include_machine_com_x64) order.push_back(Resource::machine_com_x64);
    if (request.include_x86) order.push_back(Resource::com_x86);
    for (Resource resource : {Resource::profile, Resource::keyboard_category, Resource::system_tray_category, Resource::ui_element_category})
        order.push_back(resource);
    return detail::apply_transaction(backend, before, desired, order,
        observer,TransactionOperation::install_or_repair);
}

inline LifecycleResult uninstall(Backend& backend, const InstallRequest& request,
                                 TransactionObserver* observer=nullptr) {
    if (request.owner.empty() || !detail::absolute_path(request.expected_x64_path) ||
        (request.include_x86 && !detail::absolute_path(request.expected_x86_path)) ||
        (!request.include_x86 && !request.expected_x86_path.empty())) return {E_INVALIDARG, S_OK};
    const HRESULT policy_status = detail::validate_machine_com_policy(request);
    if (policy_status != S_OK) return {policy_status, S_OK};
    std::array<ResourceState, detail::index(Resource::count)> before{};
    HRESULT status = detail::read_all(backend, before);
    if (status != S_OK) return {status, S_OK};
    // Uninstall uses only the pinned installed path, never an install target.
    auto ownership_request = request;
    ownership_request.x64_binary_path = request.expected_x64_path;
    status = detail::validate_machine_com_prestate(ownership_request, before);
    if (status != S_OK) return {status, S_OK};
    auto desired = before;
    if (request.include_machine_com_x64)
        desired[detail::index(Resource::machine_com_x64)].present = false;
    std::vector<Resource> order{Resource::com_x64};
    if (request.include_machine_com_x64) order.push_back(Resource::machine_com_x64);
    if (request.include_x86) order.push_back(Resource::com_x86);
    for (Resource resource : {Resource::ui_element_category, Resource::keyboard_category, Resource::system_tray_category, Resource::profile})
        order.push_back(resource);
    const ResourceState& x64 = before[detail::index(Resource::com_x64)];
    status = detail::ownership_status(x64, request.owner, request.expected_x64_path, request.expected_x64_path);
    if (FAILED(status)) return {status, S_OK};
    if (x64.present) desired[detail::index(Resource::com_x64)].present = false;

    if (!request.include_x86) {
        const bool com_x86_remains=before[detail::index(Resource::com_x86)].present;
        const Resource shared_resources[]={Resource::profile,Resource::keyboard_category,Resource::system_tray_category,Resource::ui_element_category};
        if (!com_x86_remains) {
            for(Resource resource:shared_resources) {
                const ResourceState& current=before[detail::index(resource)];
                if(current.present&&current.created_by_lifecycle&&current.owner==request.owner)
                    desired[detail::index(resource)].present=false;
            }
        }
        const std::function<HRESULT(Resource)> guard=[&](Resource resource)->HRESULT {
            const bool shared_resource=resource==Resource::profile||resource==Resource::keyboard_category||
                resource==Resource::system_tray_category||resource==Resource::ui_element_category;
            if(com_x86_remains||!shared_resource)
                return S_OK;
            ResourceState current_x86;
            const HRESULT read_status=backend.read(Resource::com_x86,&current_x86);
            if(read_status!=S_OK)return read_status;
            return current_x86==before[detail::index(Resource::com_x86)]
                ?S_OK:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        };
        if (request.include_machine_com_x64)
            for (size_t i=0;i<desired.size();++i)
                if (before[i].present && !desired[i].present) desired[i]=ResourceState{};
        status=backend.prepare_apply_targets(TransactionOperation::uninstall,request,before,&desired);
        if(status!=S_OK)return {status,S_OK};
        return detail::apply_transaction(backend,before,desired,order,
            observer,TransactionOperation::uninstall,guard);
    }

    const ResourceState& x86 = before[detail::index(Resource::com_x86)];
    status = detail::ownership_status(x86, request.owner, request.expected_x86_path, request.expected_x86_path);
    if (FAILED(status)) return {status, S_OK};
    if (x86.present) desired[detail::index(Resource::com_x86)].present = false;
    for (Resource resource : {Resource::profile, Resource::keyboard_category, Resource::system_tray_category, Resource::ui_element_category}) {
        const ResourceState& current = before[detail::index(resource)];
        if (current.present && current.owner != request.owner) return {HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), S_OK};
        if (current.present && current.created_by_lifecycle) desired[detail::index(resource)].present = false;
    }
    // Shared TSF state is removed only after both COM views. Borrowed shared
    // state remains intact; a failure restores all resources touched so far.
    status=backend.prepare_apply_targets(TransactionOperation::uninstall,request,before,&desired);
    if(status!=S_OK)return {status,S_OK};
    return detail::apply_transaction(backend, before, desired, order,
        observer,TransactionOperation::uninstall);
}

inline LifecycleResult install_or_repair(Backend& backend,const InstallRequest& request,
                                         TransactionObserver& observer) {
    return install_or_repair(backend,request,&observer);
}
inline LifecycleResult uninstall(Backend& backend,const InstallRequest& request,
                                 TransactionObserver& observer) {
    return uninstall(backend,request,&observer);
}

} // namespace boshiamy::installer
#endif
