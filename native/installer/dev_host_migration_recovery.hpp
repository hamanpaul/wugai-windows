#pragma once
#include "dev_host_package_migration.hpp"
namespace boshiamy::installer::dev_host_migration_recovery {
enum class Disposition { uncertain, active, reversed };
struct Report { HRESULT status = E_NOTIMPL; bool coherent = false; Disposition disposition = Disposition::uncertain; };
inline bool sidecar_presence(const std::filesystem::path& path, bool* present) {
    if (!present) return false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) return false;
    if (std::filesystem::is_symlink(status) ||
        (std::filesystem::exists(status) && files::detail::reparse(path))) return false;
    *present = std::filesystem::exists(status);
    return true;
}
// Read-only classification of this exact completed migration. It never calls
// restore, cleanup, install health, or infers ownership of retained shared state.
inline Report inspect(const durable_journal::Identity& identity, Backend& backend) {
    const auto fail = [](HRESULT status) { return Report{status, false}; };
    try {
        if (identity.schema_version != 2 || identity.owner != "LiuKai-TSF-v1" ||
            !durable_journal::detail::valid_identity(identity)) return fail(E_INVALIDARG);
        const std::filesystem::path ledger = identity.storage_root, root = identity.install_root;
        durable_journal::Store journal(ledger / L"transaction.journal", identity, ledger);
        durable_snapshot::Store snapshot(ledger / L"transaction.snapshot", identity, ledger);
        durable_journal::Document document;
        HRESULT status = journal.read(identity, &document);
        if (status != S_OK) return fail(status);
        ResourceStates before{}, current{}; std::string prestate;
        status = snapshot.read(identity, &before, &prestate);
        if (status != S_OK) return fail(status);
        const HRESULT conflict = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        using Kind = durable_journal::Event::Kind;
        if (!document.committed || !document.all_operations_succeeded || document.events.size() != 7 ||
            document.events[6].kind != Kind::commit) return fail(conflict);
        const char* resources[] = {"prestate", "file-publication", "com-x64"};
        const char* actions[] = {"capture-x64-dev-host-migration", "apply", "apply"};
        for (size_t i = 0; i != 3; ++i) {
            const auto& intent = document.events[2*i]; const auto& observed = document.events[2*i+1];
            if (intent.kind != Kind::intent || observed.kind != Kind::observation ||
                intent.intent.resource != resources[i] || intent.intent.action != actions[i] ||
                observed.observation.sequence != intent.intent.sequence || observed.observation.operation_status != S_OK ||
                observed.observation.observed_fingerprint != intent.intent.desired_fingerprint) return fail(conflict);
        }
        if (document.events[0].intent.expected_before_fingerprint != std::string(64, '0') ||
            document.events[0].intent.desired_fingerprint != prestate) return fail(conflict);
        files::DeploymentSnapshot installed;
        status = files::FileDeployment::inspect(root, identity.owner, &installed);
        if (status != S_OK) return fail(status);
        if (installed.has_x86 || installed.version != identity.package_version ||
            installed.manifest_sha256 != identity.package_manifest_sha256) return fail(conflict);
        std::string old_files, new_files;
        if (!durable_cli::detail::file_fingerprint(identity, false, nullptr, nullptr, &old_files) ||
            !durable_cli::detail::file_fingerprint(identity, true, &installed, nullptr, &new_files) ||
            document.events[2].intent.expected_before_fingerprint != old_files ||
            document.events[2].intent.desired_fingerprint != new_files) return fail(conflict);
        status = installer::detail::read_all(backend, current);
        if (status != S_OK) return fail(status);
        // A compensation is a separate, linked transaction. Its mere presence
        // makes an uncommitted or malformed state uncertain; a plain COM inverse
        // is never mistaken for a durably reversed migration.
        const auto rollback_journal_path = ledger / L"rollback.journal";
        const auto rollback_snapshot_path = ledger / L"rollback.snapshot";
        bool rollback_journal_exists = false, rollback_snapshot_exists = false;
        if (!sidecar_presence(rollback_journal_path, &rollback_journal_exists) ||
            !sidecar_presence(rollback_snapshot_path, &rollback_snapshot_exists) ||
            rollback_journal_exists != rollback_snapshot_exists) return fail(conflict);
        const bool reversed = rollback_journal_exists;
        ResourceStates effective = current;
        if (reversed) {
            durable_snapshot::Store rollback_snapshot(rollback_snapshot_path, identity, ledger);
            status = rollback_snapshot.read(identity, &effective);
            if (status != S_OK) return fail(status);
        }
        const auto& com = effective[0]; const auto& old = before[0];
        if (!com.present || !com.created_by_lifecycle || com.owner != L"LiuKai-TSF-v1" || com.threading_model != L"Apartment" ||
            !profile_display::resource_name::exact_path_equal(com.path, installed.x64_dll.wstring()) ||
            !old.present || !old.created_by_lifecycle || old.owner != com.owner || old.threading_model != L"Apartment" ||
            profile_display::resource_name::exact_path_equal(old.path, com.path)) return fail(conflict);
        for (size_t i = 1; i < effective.size(); ++i) if (effective[i] != before[i]) return fail(conflict);
        profile_display_payload::Payload profile;
        status = profile_display_payload::decode(before[2].private_data, &profile);
        if (status != S_OK || !before[2].present || !profile.has_display ||
            !profile_display_plan::detail::exact_identity(profile.display)) return fail(status == S_OK ? conflict : status);
        std::string expected_com, desired_com;
        ResourceState projected;
        status = backend.project_apply_state(Resource::com_x64, com, &projected);
        if (status != S_OK) return fail(status);
        if (!durable_observer::detail::fingerprint_state(identity, Resource::com_x64, old, &expected_com, false) ||
            !durable_observer::detail::fingerprint_state(identity, Resource::com_x64, projected, &desired_com, false) ||
            document.events[4].intent.expected_before_fingerprint != expected_com ||
            document.events[4].intent.desired_fingerprint != desired_com) return fail(conflict);
        std::string final;
        if (!durable_snapshot::detail::encode(identity, effective, &final) ||
            files::detail::sha256_bytes(final) != document.events[6].commit_fingerprint) return fail(conflict);
        // Preserve the exact source DLL as an independent migration inverse input.
        if (!files::detail::absolute_no_reparse(old.path, true) ||
            files::detail::sha256_file(old.path) != files::detail::sha256_file(installed.x64_dll)) return fail(conflict);
        const std::set<std::wstring> allowed = {L".liukai-owner",L".liukai-install-state",L"LiuKaiInstaller.exe",L"shared",L"versions"};
        for (const auto& entry : std::filesystem::directory_iterator(root))
            if (!allowed.count(entry.path().filename().wstring()) || files::detail::reparse(entry.path())) return fail(conflict);
        if (reversed) {
            if (current != before) return fail(conflict);
            std::string forward_image;
            if (!durable_journal::detail::read_file(ledger / L"transaction.journal", &forward_image)) return fail(conflict);
            const std::string forward_hash = files::detail::sha256_bytes(forward_image);
            std::string migrated_com, original_com;
            if (!durable_observer::detail::fingerprint_state(identity, Resource::com_x64, effective[0], &migrated_com) ||
                !durable_observer::detail::fingerprint_state(identity, Resource::com_x64, before[0], &original_com)) return fail(conflict);
            durable_journal::Store rollback_journal(rollback_journal_path, identity, ledger);
            durable_journal::Document rollback;
            status = rollback_journal.read(identity, &rollback);
            if (status != S_OK || !rollback.committed || !rollback.all_operations_succeeded || rollback.events.size() != 5)
                return fail(conflict);
            const auto& link = rollback.events[0].intent;
            const auto& link_observed = rollback.events[1].observation;
            const auto& restore = rollback.events[2].intent;
            const auto& restore_observed = rollback.events[3].observation;
            std::string original_image;
            if (!durable_snapshot::detail::encode(identity, before, &original_image)) return fail(conflict);
            if (rollback.events[0].kind != Kind::intent || rollback.events[1].kind != Kind::observation ||
                rollback.events[2].kind != Kind::intent || rollback.events[3].kind != Kind::observation ||
                rollback.events[4].kind != Kind::commit ||
                link.sequence != 1 || link.resource != "forward-link" || link.action != "bind" ||
                link.expected_before_fingerprint != forward_hash ||
                link.desired_fingerprint != document.events[6].commit_fingerprint ||
                link_observed.sequence != link.sequence || link_observed.operation_status != S_OK ||
                link_observed.observed_fingerprint != link.desired_fingerprint || link_observed.mutation_may_have_occurred ||
                restore.sequence != 2 || restore.resource != "com-x64" || restore.action != "restore" ||
                restore.expected_before_fingerprint != migrated_com || restore.desired_fingerprint != original_com ||
                restore_observed.sequence != restore.sequence || restore_observed.operation_status != S_OK ||
                restore_observed.observed_fingerprint != original_com || !restore_observed.mutation_may_have_occurred ||
                rollback.events[4].commit_fingerprint != files::detail::sha256_bytes(original_image)) return fail(conflict);
            return {S_FALSE, false, Disposition::reversed};
        }
        return {S_OK, true, Disposition::active};
    } catch (...) { return fail(HRESULT_FROM_WIN32(ERROR_INVALID_STATE)); }
}
}
