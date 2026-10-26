#pragma once

#include "deployment_coordinator.hpp"

#include <array>
#include <string>
#include <vector>

namespace boshiamy::installer::recovery {
namespace fs = std::filesystem;

enum class Classification {
    no_pending_journal,
    owned_journal_current_state_coherent,
    incomplete_or_ambiguous,
    unrecognized_or_foreign_journal,
    unreadable
};

struct Report {
    Classification classification = Classification::unreadable;
    HRESULT status = E_FAIL;
    bool automatic_action = false;
    bool filesystem_mutated = false;
    bool backend_mutated = false;
    std::vector<std::string> evidence;
    files::DeploymentSnapshot deployment;
};

class ProfileEnabledReader {
public:
    virtual ~ProfileEnabledReader() = default;
    virtual HRESULT read_enabled(bool* enabled) = 0;
};

inline std::wstring widen_ascii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

inline bool known_journal_inventory(const fs::path& root, const fs::path& journal,
                                    const std::string& owner) {
    try {
        if (files::detail::reparse(journal) || !fs::is_directory(journal)) return false;
        const auto name = journal.filename().wstring();
        if (name.rfind(L".txn-", 0) != 0) return false;
        const std::wstring token_w = name.substr(5);
        if (token_w.empty()) return false;
        const std::string token(token_w.begin(), token_w.end());
        if (!files::detail::safe_label(token) ||
            files::detail::read_text(journal / L".txn-owner") != owner + "\n" + token + "\n") return false;

        const std::set<std::wstring> top = {
            L".txn-owner", L"version", L"new-cli", L"new-branding", L"old-cli",
            L"old-branding", L"old-state", L"new-state", L"new-owner"
        };
        std::error_code ec;
        for (fs::recursive_directory_iterator it(journal, fs::directory_options::none, ec), end;
             it != end && !ec; it.increment(ec)) {
            const auto path = it->path();
            if (files::detail::reparse(path)) return false;
            const auto relative = path.lexically_relative(journal).generic_wstring();
            if (relative.empty() || relative.find(L"..") != std::wstring::npos) return false;
            const auto slash = relative.find(L'/');
            if (slash == std::wstring::npos) {
                if (!top.count(relative)) return false;
                if (relative == L"version") {
                    if (!it->is_directory(ec) || ec) return false;
                } else if (!it->is_regular_file(ec) || ec) return false;
                continue;
            }
            if (relative.substr(0, slash) != L"version") return false;
            const std::wstring tail = relative.substr(slash + 1);
            if (tail == L".liukai-version-owner" || tail == L"package-manifest.json") {
                if (!it->is_regular_file(ec) || ec) return false;
            } else if (tail == L"x64" || tail == L"x86") {
                if (!it->is_directory(ec) || ec) return false;
            } else {
                const auto arch_slash = tail.find(L'/');
                if (arch_slash == std::wstring::npos) return false;
                const auto arch = tail.substr(0, arch_slash);
                const auto file = tail.substr(arch_slash + 1);
                if ((arch != L"x64" && arch != L"x86") || file.empty() ||
                    file.find(L'/') != std::wstring::npos || it->is_directory(ec) || ec) return false;
                bool recognized = false;
                for (const auto& expected : files::detail::arch_files())
                    if (file == files::detail::widen_ascii(expected)) recognized = true;
                if (!recognized) return false;
            }
        }
        return !ec && root == journal.parent_path();
    } catch (...) {
        return false;
    }
}

inline Report inspect(const fs::path& install_root, const std::string& owner,
                      Backend& backend, ProfileEnabledReader& profile_enabled) {
    Report report;
    try {
        if (!files::detail::safe_label(owner) || !install_root.is_absolute()) {
            report.classification = Classification::unreadable;
            report.status = E_INVALIDARG;
            report.evidence.push_back("install root or owner is invalid");
            return report;
        }
        std::error_code ec;
        const bool root_exists = fs::exists(install_root, ec);
        if (ec) {
            report.classification = Classification::unreadable;
            report.status = HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
            report.evidence.push_back("cannot query install root");
            return report;
        }
        if (!root_exists) {
            report.classification = Classification::no_pending_journal;
            report.status = S_OK;
            report.evidence.push_back("install root is absent");
            return report;
        }
        if (!fs::is_directory(install_root, ec) || ec || files::detail::reparse(install_root)) {
            report.classification = Classification::unreadable;
            report.status = E_ACCESSDENIED;
            report.evidence.push_back("install root is not a plain directory");
            return report;
        }

        std::vector<fs::path> journals;
        for (fs::directory_iterator it(install_root, fs::directory_options::none, ec), end;
             it != end && !ec; it.increment(ec)) {
            const auto entry = it->path();
            const auto name = entry.filename().wstring();
            if (name.rfind(L".txn-", 0) != 0) continue;
            journals.push_back(entry);
        }
        if (ec) {
            report.classification = Classification::unreadable;
            report.status = HRESULT_FROM_WIN32(static_cast<DWORD>(ec.value()));
            report.evidence.push_back("cannot enumerate transaction journals");
            return report;
        }
        if (journals.empty()) {
            report.classification = Classification::no_pending_journal;
            report.status = S_OK;
            report.evidence.push_back("no transaction journal exists");
            return report;
        }
        if (journals.size() != 1) {
            report.classification = Classification::incomplete_or_ambiguous;
            report.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            report.evidence.push_back("multiple transaction journals have no durable ordering record");
            return report;
        }
        if (!known_journal_inventory(install_root, journals.front(), owner)) {
            report.classification = Classification::unrecognized_or_foreign_journal;
            report.status = E_ACCESSDENIED;
            report.evidence.push_back("journal owner marker, path, or inventory is not recognized");
            return report;
        }

        const HRESULT file_status = files::FileDeployment::inspect(install_root, owner, &report.deployment);
        if (FAILED(file_status)) {
            report.classification = Classification::incomplete_or_ambiguous;
            report.status = file_status;
            report.evidence.push_back("current install state cannot be proven from its owner marker and hashes");
            return report;
        }

        std::array<ResourceState, detail::index(Resource::count)> states{};
        const HRESULT read_status = detail::read_all(backend, states);
        if (read_status != S_OK) {
            report.classification = Classification::unreadable;
            report.status = read_status;
            report.evidence.push_back("one or more COM/TSF resources could not be read");
            return report;
        }
        const auto expected_owner = widen_ascii(owner);
        const ResourceState& x64 = states[detail::index(Resource::com_x64)];
        const ResourceState& x86 = states[detail::index(Resource::com_x86)];
        auto owned_com_matches = [&](const ResourceState& current, const fs::path& expected) {
            return current.present && current.owner == expected_owner &&
                   current.threading_model == L"Apartment" &&
                   detail::same_path(current.path, expected.wstring());
        };
        if (!owned_com_matches(x64, report.deployment.x64_dll) ||
            !owned_com_matches(x86, report.deployment.x86_dll)) {
            report.classification = Classification::incomplete_or_ambiguous;
            report.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            report.evidence.push_back("both COM views do not point to the current owned version");
            return report;
        }

        const ResourceState& profile = states[detail::index(Resource::profile)];
        const ResourceState& keyboard = states[detail::index(Resource::keyboard_category)];
        const ResourceState& tray = states[detail::index(Resource::system_tray_category)];
        const ResourceState& ui_element = states[detail::index(Resource::ui_element_category)];
        if (!profile.present || !keyboard.present || !tray.present || !ui_element.present ||
            (profile.created_by_lifecycle &&
             (profile.owner != expected_owner || !detail::same_path(profile.path, report.deployment.branding_dll.wstring()))) ||
            (keyboard.created_by_lifecycle && keyboard.owner != expected_owner) ||
            (tray.created_by_lifecycle && tray.owner != expected_owner) ||
            (ui_element.created_by_lifecycle && ui_element.owner != expected_owner)) {
            report.classification = Classification::incomplete_or_ambiguous;
            report.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
            report.evidence.push_back("shared profile or required category state is absent or inconsistent");
            return report;
        }
        if (profile.created_by_lifecycle) {
            bool enabled = false;
            const HRESULT enabled_status = profile_enabled.read_enabled(&enabled);
            if (FAILED(enabled_status)) {
                report.classification = Classification::unreadable;
                report.status = enabled_status;
                report.evidence.push_back("cannot read enabled state for the lifecycle-owned profile");
                return report;
            }
            if (!enabled) {
                report.classification = Classification::incomplete_or_ambiguous;
                report.status = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
                report.evidence.push_back("lifecycle-owned profile is disabled");
                return report;
            }
        }

        report.classification = Classification::owned_journal_current_state_coherent;
        report.status = S_OK;
        report.evidence.push_back("install files and current COM/TSF state are coherent with one owned journal");
        report.evidence.push_back("the journal has no durable transaction phase or pre-state snapshot; cleanup direction is unknown");
        report.evidence.push_back("no automatic cleanup or rollback was performed");
        return report;
    } catch (...) {
        report.classification = Classification::unreadable;
        report.status = E_FAIL;
        report.evidence.push_back("unexpected read-only recovery inspection failure");
        return report;
    }
}

} // namespace boshiamy::installer::recovery
