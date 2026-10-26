#pragma once

#include "dev_host_migration_recovery.hpp"
#include "dev_host_migration.hpp"

namespace boshiamy::installer::dev_host_migration_rollback {
namespace fs = std::filesystem;
struct ExpectedInputPins {
    std::string forward_journal_sha256;
    std::string original_snapshot_sha256;
};

// A committed forward migration remains immutable. The compensation uses a
// separate snapshot/journal inside that exact identity's external ledger.
class RestoreObserver final : public TransactionObserver {
public:
    RestoreObserver(durable_journal::Store& journal, const durable_journal::Identity& identity)
        : journal_(journal), identity_(identity) {}
    HRESULT begin(TransactionOperation, const ResourceStates&) override { return E_NOTIMPL; }
    HRESULT before(Resource resource, TransactionMutation mutation,
                   const ResourceState& expected, const ResourceState& desired) override {
        if (pending_ || resource != Resource::com_x64 || mutation != TransactionMutation::restore)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string before_hash, desired_hash;
        if (!durable_observer::detail::fingerprint_state(identity_, resource, expected, &before_hash) ||
            !durable_observer::detail::fingerprint_state(identity_, resource, desired, &desired_hash))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        const HRESULT status = journal_.append_intent("com-x64", "restore", before_hash, desired_hash, &sequence_);
        if (status == S_OK) { pending_ = true; desired_hash_ = std::move(desired_hash); }
        return status;
    }
    HRESULT after(Resource resource, TransactionMutation mutation, HRESULT operation,
                  HRESULT readback, const ResourceState* observed, bool may_mutate) override {
        if (!pending_ || resource != Resource::com_x64 || mutation != TransactionMutation::restore)
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        std::string fingerprint = durable_observer::detail::unknown_fingerprint();
        if (readback == S_OK && observed &&
            !durable_observer::detail::fingerprint_state(identity_, resource, *observed, &fingerprint))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        HRESULT recorded = operation != S_OK ? operation : readback != S_OK ? readback :
            !observed || fingerprint != desired_hash_ ? HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) : S_OK;
        const HRESULT status = journal_.append_observation(sequence_, recorded, fingerprint, may_mutate);
        if (status == S_OK) pending_ = false;
        return status;
    }
    bool pending() const { return pending_; }
private:
    durable_journal::Store& journal_;
    durable_journal::Identity identity_;
    uint64_t sequence_ = 0;
    std::string desired_hash_;
    bool pending_ = false;
};

inline LifecycleResult execute(const durable_journal::Identity& identity, Backend& backend,
                               const ExpectedInputPins* expected = nullptr) {
    const auto conflict = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    try {
        if (identity.schema_version != 2 || identity.owner != "LiuKai-TSF-v1" ||
            !durable_journal::detail::valid_identity(identity)) return {E_INVALIDARG, S_OK};
        const fs::path ledger = identity.storage_root;
        const fs::path target = identity.install_root;
        if (expected && (!files::detail::valid_hash(expected->forward_journal_sha256) ||
                         !files::detail::valid_hash(expected->original_snapshot_sha256)))
            return {E_INVALIDARG, S_OK};
        if (!durable_journal::detail::same_path(ledger.wstring(),
                (dev_host_migration_history::ledger_base(target) / files::detail::widen_ascii(identity.token)).wstring()))
            return {E_INVALIDARG, S_OK};
        dev_host_migration_history::Lock lock;
        HRESULT status = lock.acquire();
        if (status != S_OK) return {status, S_OK};
        const auto pins_match = [&]() {
            return !expected ||
                (files::detail::absolute_no_reparse(ledger / L"transaction.journal", true) &&
                 files::detail::absolute_no_reparse(ledger / L"transaction.snapshot", true) &&
                 files::detail::sha256_file(ledger / L"transaction.journal") == expected->forward_journal_sha256 &&
                 files::detail::sha256_file(ledger / L"transaction.snapshot") == expected->original_snapshot_sha256);
        };
        if (!pins_match()) return {conflict, S_OK};
        const auto report = dev_host_migration_recovery::inspect(identity, backend);
        if (report.disposition == dev_host_migration_recovery::Disposition::reversed)
            return pins_match() ? LifecycleResult{S_FALSE, S_OK} : LifecycleResult{conflict, S_OK};
        if (report.disposition != dev_host_migration_recovery::Disposition::active)
            return {report.status == S_OK ? conflict : report.status, S_OK};
        // An unrelated unresolved upgrade in this parent makes the target
        // ambiguous. Inspect before creating a compensation sidecar.
        status = dev_host_migration_history::inspect(target);
        if (status != S_OK) return {status, S_OK};
        const fs::path rollback_journal_path = ledger / L"rollback.journal";
        const fs::path rollback_snapshot_path = ledger / L"rollback.snapshot";
        bool journal_present = false, snapshot_present = false;
        if (!dev_host_migration_recovery::sidecar_presence(rollback_journal_path, &journal_present) ||
            !dev_host_migration_recovery::sidecar_presence(rollback_snapshot_path, &snapshot_present) ||
            journal_present || snapshot_present) return {conflict, S_OK};
        durable_snapshot::Store original_snapshot(ledger / L"transaction.snapshot", identity, ledger);
        ResourceStates original{}, migrated{};
        status = original_snapshot.read(identity, &original);
        if (status != S_OK) return {status, S_OK};
        status = installer::detail::read_all(backend, migrated);
        if (status != S_OK) return {status, S_OK};
        if (migrated == original) return {conflict, S_OK};
        std::string forward_image;
        if (!durable_journal::detail::read_file(ledger / L"transaction.journal", &forward_image))
            return {HRESULT_FROM_WIN32(GetLastError()), S_OK};
        durable_journal::Store forward_journal(ledger / L"transaction.journal", identity, ledger);
        durable_journal::Document forward;
        status = forward_journal.read(identity, &forward);
        if (status != S_OK || forward.events.size() != 7 || !forward.committed)
            return {status == S_OK ? conflict : status, S_OK};
        std::string migrated_image;
        if (!durable_snapshot::detail::encode(identity, migrated, &migrated_image) ||
            files::detail::sha256_bytes(migrated_image) != forward.events[6].commit_fingerprint)
            return {conflict, S_OK};
        std::string stable_forward_image;
        if (!durable_journal::detail::read_file(ledger / L"transaction.journal", &stable_forward_image) ||
            stable_forward_image != forward_image) return {conflict, S_OK};
        const auto file_inputs_match = [&]() {
            std::string image;
            if (!durable_journal::detail::read_file(ledger / L"transaction.journal", &image) ||
                image != forward_image || !pins_match() ||
                !files::detail::absolute_no_reparse(original[0].path, true)) return false;
            files::DeploymentSnapshot installed;
            if (files::FileDeployment::inspect(target, identity.owner, &installed) != S_OK ||
                installed.has_x86 || installed.version != identity.package_version ||
                installed.manifest_sha256 != identity.package_manifest_sha256 ||
                !profile_display::resource_name::exact_path_equal(migrated[0].path, installed.x64_dll.wstring()))
                return false;
            const auto source_hash = files::detail::sha256_file(original[0].path);
            return !source_hash.empty() && source_hash == files::detail::sha256_file(installed.x64_dll);
        };
        if (!file_inputs_match()) return {conflict, S_OK};
        // Refresh the complete CAS after file/journal validation, before any
        // compensation file is written. The restore helper repeats the CAS.
        ResourceStates refreshed{};
        status = installer::detail::read_all(backend, refreshed);
        if (status != S_OK || refreshed != migrated || !pins_match())
            return {status == S_OK ? conflict : status, S_OK};
        durable_snapshot::Store rollback_snapshot(rollback_snapshot_path, identity, ledger);
        status = rollback_snapshot.create(migrated);
        if (status != S_OK) return {status, S_OK};
        durable_journal::Store rollback_journal(rollback_journal_path, identity, ledger);
        status = rollback_journal.create();
        if (status != S_OK) return {status, S_OK};
        const std::string forward_hash = files::detail::sha256_bytes(forward_image);
        uint64_t sequence = 0;
        status = rollback_journal.append_intent("forward-link", "bind", forward_hash,
                                                forward.events[6].commit_fingerprint, &sequence);
        if (status != S_OK) return {status, S_OK};
        status = rollback_journal.append_observation(sequence, S_OK,
                        forward.events[6].commit_fingerprint, false);
        if (status != S_OK) return {status, S_OK};
        if (!file_inputs_match()) return {conflict, S_OK};
        RestoreObserver observer(rollback_journal, identity);
        const LifecycleResult restored = dev_host_migration::detail::restore_verified(
            backend, original, migrated, &observer, S_OK);
        if (restored.operation != S_OK || restored.cleanup != S_OK || observer.pending()) return restored;
        ResourceStates after{};
        status = installer::detail::read_all(backend, after);
        if (status != S_OK || after != original) return {conflict, status == S_OK ? conflict : status};
        std::string image;
        if (!durable_snapshot::detail::encode(identity, original, &image)) return {E_FAIL, E_FAIL};
        status = rollback_journal.append_commit(files::detail::sha256_bytes(image));
        if (status != S_OK) return {S_OK, status};
        const auto verified = dev_host_migration_recovery::inspect(identity, backend);
        return verified.disposition == dev_host_migration_recovery::Disposition::reversed &&
                       verified.status == S_FALSE ? LifecycleResult{S_OK, S_OK} : LifecycleResult{S_OK, conflict};
    } catch (...) { return {HRESULT_FROM_WIN32(ERROR_INVALID_STATE), S_OK}; }
}
} // namespace boshiamy::installer::dev_host_migration_rollback
