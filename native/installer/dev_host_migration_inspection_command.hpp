#pragma once

#include "dev_host_migration_rollback_command.hpp"
#include "dev_host_migration_history.hpp"
#include "durable_compensation_marker.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace boshiamy::installer::dev_host_migration_inspection_command {
namespace fs = std::filesystem;

struct TerminalLedgerRecord {
    durable_journal::Identity identity;
    std::string forward_journal_sha256;
    std::string original_snapshot_sha256;
    std::string terminal_marker_sha256;
    bool prestate_exact = false;
    bool file_rollback_exact = false;
};

struct Result {
    HRESULT status = E_UNEXPECTED;
    durable_journal::Identity identity;
    dev_host_migration_recovery::Report verification{};
    bool history_checked = false;
    HRESULT history_status = E_NOTIMPL;
    std::vector<TerminalLedgerRecord> terminal_ledgers;
};

inline HRESULT profile_state_equal(const ResourceState& expected, const ResourceState& actual,
                                   bool* equal) {
    if (!equal) return E_POINTER;
    *equal = false;
    if (expected.present != actual.present ||
        expected.created_by_lifecycle != actual.created_by_lifecycle ||
        expected.owner != actual.owner || expected.path != actual.path ||
        expected.threading_model != actual.threading_model) return S_OK;
    profile_display_payload::Payload expected_payload, actual_payload;
    HRESULT status = profile_display_payload::decode(expected.private_data, &expected_payload);
    if (status != S_OK) return status;
    status = profile_display_payload::decode(actual.private_data, &actual_payload);
    if (status != S_OK) return status;
    if (expected_payload.has_display) {
        *equal = actual_payload.has_display && expected.private_data == actual.private_data;
        return S_OK;
    }
    // Older package ledgers contain the shared-profile legacy bytes only;
    // DisplayBackend wraps those same bytes together with current PDE1 data.
    *equal = expected_payload.legacy == actual_payload.legacy;
    return S_OK;
}

inline HRESULT exact_six_resource_prestate(const ResourceStates& expected,
                                           const ResourceStates& actual,
                                           bool* equal) {
    if (!equal) return E_POINTER;
    *equal = false;
    bool same_profile = false;
    HRESULT status = profile_state_equal(expected[static_cast<size_t>(Resource::profile)],
                                         actual[static_cast<size_t>(Resource::profile)],
                                         &same_profile);
    if (status != S_OK || !same_profile) return status;
    for (size_t i = 0; i != expected.size(); ++i) {
        if (i == static_cast<size_t>(Resource::profile)) continue;
        if (expected[i] != actual[i]) return S_OK;
    }
    *equal = true;
    return S_OK;
}

inline HRESULT collect_terminal_ledgers(const fs::path& root, const ResourceStates& current,
                                        std::vector<TerminalLedgerRecord>* output) {
    if (!output) return E_POINTER;
    output->clear();
    std::vector<TerminalLedgerRecord> validated;
    const HRESULT conflict = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    try {
        const fs::path base = dev_host_migration_history::ledger_base(root);
        std::error_code error;
        if (!fs::exists(base, error)) return error ? HRESULT_FROM_WIN32(ERROR_INVALID_STATE) : S_OK;
        if (error || !files::detail::absolute_no_reparse(base, true) || !fs::is_directory(base))
            return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        size_t count = 0;
        for (fs::directory_iterator it(base, fs::directory_options::none, error), end;
             it != end && !error; it.increment(error)) {
            if (++count > 1024 || !files::detail::absolute_no_reparse(it->path(), true) ||
                !it->is_directory()) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            const fs::path ledger = it->path();
            const fs::path terminal = ledger / L"terminal-compensation.journal";
            const auto terminal_status = fs::symlink_status(terminal, error);
            if (error == std::errc::no_such_file_or_directory) { error.clear(); continue; }
            if (error || fs::is_symlink(terminal_status) ||
                (fs::exists(terminal_status) && files::detail::reparse(terminal)))
                return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            if (!fs::exists(terminal_status)) continue;

            const fs::path forward = ledger / L"transaction.journal";
            const fs::path snapshot = ledger / L"transaction.snapshot";
            if (!files::detail::absolute_no_reparse(forward, true) ||
                !files::detail::absolute_no_reparse(snapshot, true) ||
                !files::detail::absolute_no_reparse(terminal, true)) return E_ACCESSDENIED;
            std::string forward_image;
            if (!durable_journal::detail::read_file(forward, &forward_image))
                return HRESULT_FROM_WIN32(GetLastError());
            size_t offset = 0;
            durable_journal::detail::Digest chain{};
            durable_journal::Identity identity;
            if (!durable_journal::detail::parse_header(forward_image, &offset, &identity, &chain) ||
                identity.schema_version != 2 || identity.owner != "LiuKai-TSF-v1" ||
                identity.token != ledger.filename().string() ||
                !durable_journal::detail::valid_identity(identity) ||
                !durable_journal::detail::same_path(identity.install_root, root.wstring()) ||
                !durable_journal::detail::same_path(identity.storage_root, ledger.wstring())) return conflict;
            TerminalLedgerRecord record;
            record.identity = identity;
            record.forward_journal_sha256 = files::detail::sha256_bytes(forward_image);
            record.original_snapshot_sha256 = files::detail::sha256_file(snapshot);
            record.terminal_marker_sha256 = files::detail::sha256_file(terminal);
            if (!files::detail::valid_hash(record.forward_journal_sha256) ||
                !files::detail::valid_hash(record.original_snapshot_sha256) ||
                !files::detail::valid_hash(record.terminal_marker_sha256)) return conflict;
            durable_compensation_marker::Facts facts;
            HRESULT status = durable_compensation_marker::collect(identity, root, &facts);
            if (status != S_OK || durable_compensation_marker::inspect(identity, root) != S_OK) return conflict;
            status = exact_six_resource_prestate(facts.prestate, current, &record.prestate_exact);
            if (status != S_OK || !record.prestate_exact) return status == S_OK ? conflict : status;
            // collect() and inspect() jointly bind the marker to the exact old
            // deployment fingerprint and the current root. S_OK is the proof.
            record.file_rollback_exact = true;
            validated.push_back(std::move(record));
        }
        if (error) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        *output = std::move(validated);
        return S_OK;
    } catch (...) { return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
}

inline bool input_pins_match(const InstallerCommand& command,
                             const durable_journal::Identity& identity) {
    try {
        const fs::path ledger = identity.storage_root;
        const fs::path journal = ledger / L"transaction.journal";
        const fs::path snapshot = ledger / L"transaction.snapshot";
        return files::detail::absolute_no_reparse(journal, true) &&
            files::detail::absolute_no_reparse(snapshot, true) &&
            files::detail::sha256_file(journal) == command.expected_forward_journal_sha256 &&
            files::detail::sha256_file(snapshot) == command.expected_original_snapshot_sha256;
    } catch (...) { return false; }
}

inline bool terminal_pins_match(const std::vector<TerminalLedgerRecord>& records) {
    try {
        for (const auto& record : records) {
            const fs::path ledger = record.identity.storage_root;
            const fs::path journal = ledger / L"transaction.journal";
            const fs::path snapshot = ledger / L"transaction.snapshot";
            const fs::path marker = ledger / L"terminal-compensation.journal";
            if (!files::detail::absolute_no_reparse(journal, true) ||
                !files::detail::absolute_no_reparse(snapshot, true) ||
                !files::detail::absolute_no_reparse(marker, true) ||
                files::detail::sha256_file(journal) != record.forward_journal_sha256 ||
                files::detail::sha256_file(snapshot) != record.original_snapshot_sha256 ||
                files::detail::sha256_file(marker) != record.terminal_marker_sha256) return false;
        }
        return true;
    } catch (...) { return false; }
}

inline Result execute(const InstallerCommand& command, Backend& backend) {
    Result result;
    const HRESULT conflict = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    try {
        dev_host_migration_history::Lock lock;
        HRESULT status = lock.acquire();
        if (status != S_OK) { result.terminal_ledgers.clear(); result.status = status; return result; }
        status = dev_host_migration_rollback_command::load_identity(
            command, &result.identity, InstallerAction::inspect_dev_host_migration);
        if (status != S_OK) { result.status = status; return result; }
        result.verification = dev_host_migration_recovery::inspect(result.identity, backend);
        const auto disposition = result.verification.disposition;
        if (disposition == dev_host_migration_recovery::Disposition::reversed &&
            result.verification.status == S_FALSE) {
            result.status = input_pins_match(command, result.identity) ? S_OK : conflict;
            if (result.status == S_OK) return result;
            return result;
        }
        if (disposition != dev_host_migration_recovery::Disposition::active ||
            result.verification.status != S_OK || !result.verification.coherent) {
            result.status = result.verification.status == S_OK ? conflict : result.verification.status;
            return result;
        }
        result.history_checked = true;
        result.history_status = dev_host_migration_history::inspect(result.identity.install_root);
        if (result.history_status != S_OK) { result.status = result.history_status; return result; }
        ResourceStates current{};
        status = installer::detail::read_all(backend, current);
        if (status != S_OK) { result.status = status; return result; }
        status = collect_terminal_ledgers(result.identity.install_root, current,
                                          &result.terminal_ledgers);
        if (status != S_OK) { result.status = status; return result; }
        // Recheck all caller pins and repeat the full native/file inspection
        // immediately before returning the read-only report.
        if (!input_pins_match(command, result.identity) ||
            !terminal_pins_match(result.terminal_ledgers)) {
            result.terminal_ledgers.clear(); result.status = conflict; return result;
        }
        ResourceStates final_state{};
        status = installer::detail::read_all(backend, final_state);
        if (status != S_OK || final_state != current) {
            result.terminal_ledgers.clear();
            result.status = status == S_OK ? conflict : status;
            return result;
        }
        const auto final_verification = dev_host_migration_recovery::inspect(result.identity, backend);
        if (final_verification.status != S_OK ||
            final_verification.disposition != dev_host_migration_recovery::Disposition::active ||
            !final_verification.coherent) {
            result.terminal_ledgers.clear(); result.status = conflict; return result;
        }
        result.status = S_OK;
        return result;
    } catch (...) {
        result.terminal_ledgers.clear();
        result.status = HRESULT_FROM_WIN32(ERROR_INVALID_STATE); return result;
    }
}

} // namespace boshiamy::installer::dev_host_migration_inspection_command
