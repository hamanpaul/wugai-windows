#pragma once

#include "command_line.hpp"
#include "dev_host_migration_rollback.hpp"

namespace boshiamy::installer::dev_host_migration_rollback_command {
namespace fs = std::filesystem;

struct Result {
    LifecycleResult transaction{E_NOTIMPL, S_OK};
    dev_host_migration_recovery::Report verification{};
    durable_journal::Identity identity{};
};

inline HRESULT load_identity(const InstallerCommand& command, durable_journal::Identity* output,
                             InstallerAction allowed_action = InstallerAction::rollback_dev_host_migration) {
    if (!output) return E_POINTER;
    *output = {};
    const auto conflict = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    try {
        const fs::path root = command.deployment.install_root;
        const fs::path ledger = command.rollback_ledger_dir;
        if (command.action != allowed_action ||
            command.deployment.owner != "LiuKai-TSF-v1" || !root.is_absolute() || !ledger.is_absolute() ||
            command.rollback_token.size() != 32 ||
            !std::all_of(command.rollback_token.begin(), command.rollback_token.end(),
                [](char c) { return (c>='0'&&c<='9') || (c>='a'&&c<='f'); }) ||
            !files::detail::valid_hash(command.expected_forward_journal_sha256) ||
            !files::detail::valid_hash(command.expected_original_snapshot_sha256) ||
            ledger.filename().wstring() != files::detail::widen_ascii(command.rollback_token) ||
            !durable_journal::detail::same_path(ledger.wstring(),
                (dev_host_migration_history::ledger_base(root) /
                 files::detail::widen_ascii(command.rollback_token)).wstring())) return E_INVALIDARG;
        if (!files::detail::absolute_no_reparse(root, true) ||
            !files::detail::absolute_no_reparse(ledger, true)) return E_ACCESSDENIED;
        const fs::path forward = ledger / L"transaction.journal";
        const fs::path original = ledger / L"transaction.snapshot";
        if (!files::detail::absolute_no_reparse(forward, true) ||
            !files::detail::absolute_no_reparse(original, true)) return E_ACCESSDENIED;
        std::string forward_image;
        if (!durable_journal::detail::read_file(forward, &forward_image))
            return HRESULT_FROM_WIN32(GetLastError());
        if (files::detail::sha256_bytes(forward_image) != command.expected_forward_journal_sha256 ||
            files::detail::sha256_file(original) != command.expected_original_snapshot_sha256)
            return conflict;
        size_t at = 0;
        durable_journal::detail::Digest chain{};
        durable_journal::Identity identity;
        if (!durable_journal::detail::parse_header(forward_image, &at, &identity, &chain) ||
            identity.schema_version != 2 || identity.owner != "LiuKai-TSF-v1" ||
            identity.token != command.rollback_token ||
            !durable_journal::detail::same_path(identity.install_root, root.wstring()) ||
            !durable_journal::detail::same_path(identity.storage_root, ledger.wstring())) return conflict;
        *output = std::move(identity);
        return S_OK;
    } catch (...) { return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
}

inline Result execute(const InstallerCommand& command, Backend& backend) {
    Result result;
    const HRESULT loaded = load_identity(command, &result.identity);
    if (loaded != S_OK) { result.transaction = {loaded, S_OK}; return result; }
    const dev_host_migration_rollback::ExpectedInputPins pins{
        command.expected_forward_journal_sha256, command.expected_original_snapshot_sha256};
    result.transaction = dev_host_migration_rollback::execute(result.identity, backend, &pins);
    result.verification = dev_host_migration_recovery::inspect(result.identity, backend);
    return result;
}

inline int exit_code(const Result& result, HRESULT observed) {
    return (result.transaction.operation == S_OK || result.transaction.operation == S_FALSE) &&
           result.transaction.cleanup == S_OK && observed == S_OK &&
           result.verification.status == S_FALSE &&
           result.verification.disposition == dev_host_migration_recovery::Disposition::reversed ? 0 : 1;
}
} // namespace boshiamy::installer::dev_host_migration_rollback_command
