#pragma once
#include "durable_transaction_observer.hpp"
#include "file_deployment.hpp"
#include "durable_compensation_marker.hpp"

namespace boshiamy::installer::dev_host_migration_history {
namespace fs = std::filesystem;
inline fs::path ledger_base(const fs::path& root) {
    return root.lexically_normal().parent_path() / L".liukai-installer-ledger" / L"LiuKai-TSF-v1";
}
struct Lock {
    HANDLE handle = nullptr;
    bool owned = false;
    ~Lock() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
    HRESULT acquire() {
        handle = CreateMutexW(nullptr, FALSE, L"Global\\LiuKai-7254A21F-ProfileDisplayMetadata");
        if (!handle) return HRESULT_FROM_WIN32(GetLastError());
        const DWORD result = WaitForSingleObject(handle, 0);
        if (result == WAIT_ABANDONED) { owned = true; return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
        if (result == WAIT_OBJECT_0) { owned = true; return S_OK; }
        return result == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY) : HRESULT_FROM_WIN32(GetLastError());
    }
};
// The installation root may already have been rolled back while its external
// journal still owns uncertain COM recovery. Never infer absence from root alone.
inline HRESULT inspect(const fs::path& root) {
    try {
        const HRESULT uncertain = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        const auto base = ledger_base(root);
        std::error_code error;
        if (!fs::exists(base, error)) return error ? uncertain : S_OK;
        if (error || !files::detail::absolute_no_reparse(base, true) || !fs::is_directory(base)) return uncertain;
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(base)) {
            if (++count > 1024 || !files::detail::absolute_no_reparse(entry.path(), true) || !entry.is_directory()) return uncertain;
            // A reversed or pending migration cannot be silently reused by a
            // later forward deployment. Its linked compensation is classified
            // only by the exact migration recovery inspector.
            std::error_code rollback_error;
            const auto journal_status = fs::symlink_status(entry.path() / L"rollback.journal", rollback_error);
            if (rollback_error && rollback_error != std::errc::no_such_file_or_directory) return uncertain;
            rollback_error.clear();
            const auto snapshot_status = fs::symlink_status(entry.path() / L"rollback.snapshot", rollback_error);
            if (rollback_error && rollback_error != std::errc::no_such_file_or_directory) return uncertain;
            if (fs::exists(journal_status) || fs::exists(snapshot_status) ||
                fs::is_symlink(journal_status) || fs::is_symlink(snapshot_status)) return uncertain;
            std::string image;
            if (!durable_journal::detail::read_file(entry.path() / L"transaction.journal", &image)) return uncertain;
            size_t at = 0;
            durable_journal::Identity identity;
            durable_journal::detail::Digest chain{};
            if (!durable_journal::detail::parse_header(image, &at, &identity, &chain) ||
                identity.schema_version != 2 || identity.owner != "LiuKai-TSF-v1" ||
                !durable_journal::detail::same_path(identity.storage_root, entry.path().wstring())) return uncertain;
            durable_journal::Store journal(entry.path() / L"transaction.journal", identity, entry.path());
            durable_journal::Document document;
            rollback_error.clear();
            const fs::path terminal_path=entry.path() / L"terminal-compensation.journal";
            const auto terminal_status=fs::symlink_status(terminal_path,rollback_error);
            if (rollback_error && rollback_error!=std::errc::no_such_file_or_directory) return uncertain;
            if (fs::is_symlink(terminal_status) ||
                (fs::exists(terminal_status) && files::detail::reparse(terminal_path))) return uncertain;
            const bool terminal_present=fs::exists(terminal_status);
            if (terminal_present) {
                if (!durable_journal::detail::same_path(identity.install_root,root.wstring()) ||
                    journal.read(identity,&document)!=S_OK || document.committed ||
                    durable_compensation_marker::inspect(identity,root)!=S_OK) return uncertain;
                continue;
            }
            if (journal.read(identity, &document) != S_OK || !document.committed || !document.all_operations_succeeded ||
                document.events.size() < 3 || document.events.front().kind != durable_journal::Event::Kind::intent ||
                document.events.front().intent.resource != "prestate" ||
                document.events.front().intent.expected_before_fingerprint != std::string(64, '0') ||
                (document.events.front().intent.action != "capture-x64-dev-host-migration" &&
                 document.events.front().intent.action != "capture-install-or-repair")) return uncertain;
            for (const auto& event : document.events)
                if (event.kind == durable_journal::Event::Kind::observation && event.observation.operation_status != S_OK)
                    return uncertain;
            durable_snapshot::Store snapshot(entry.path() / L"transaction.snapshot", identity, entry.path());
            ResourceStates original{}; std::string fingerprint;
            if (snapshot.read(identity, &original, &fingerprint) != S_OK ||
                fingerprint != document.events.front().intent.desired_fingerprint) return uncertain;
        }
        return S_OK;
    } catch (...) { return HRESULT_FROM_WIN32(ERROR_INVALID_STATE); }
}
}
