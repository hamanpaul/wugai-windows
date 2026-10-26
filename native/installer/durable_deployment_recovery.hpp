#pragma once

#include "deployment_recovery.hpp"
#include "durable_cli_session.hpp"

#include <string>
#include <vector>

namespace boshiamy::installer::durable_recovery {
namespace fs = std::filesystem;
namespace journal = boshiamy::installer::durable_journal;
namespace snapshot = boshiamy::installer::durable_snapshot;

enum class Classification {
    file_published_tsf_unchanged,
    committed_x64_singleton_coherent,
    incomplete_or_ambiguous,
    unreadable
};

struct Report {
    Classification classification = Classification::unreadable;
    HRESULT status = E_FAIL;
    bool automatic_action = false;
    bool filesystem_mutated = false;
    bool backend_mutated = false;
    std::vector<std::string> evidence;
    ResourceStates pre_state{};
    files::DeploymentSnapshot current_files;
};

inline Report inspect_file_published_before_tsf(const journal::Identity& identity,
                                                Backend& backend) {
    Report report;
    auto ambiguous = [&](HRESULT status, const char* reason) {
        report.classification = Classification::incomplete_or_ambiguous;
        report.status = status;
        report.evidence.push_back(reason);
        return report;
    };
    try {
        if (!journal::detail::valid_identity(identity) || (identity.schema_version != 2 && identity.schema_version != 3)) {
            report.classification = Classification::unreadable;
            report.status = E_INVALIDARG;
            report.evidence.push_back("durable transaction identity is invalid");
            return report;
        }
        const fs::path root(identity.install_root);
        const fs::path ledger(identity.storage_root);
        const fs::path journal_path = ledger / L"transaction.journal";
        const fs::path snapshot_path = ledger / L"transaction.snapshot";
        journal::Store journal_store(journal_path, identity, ledger);
        journal::Document document;
        HRESULT status = journal_store.read(identity, &document);
        if (FAILED(status)) return ambiguous(status, "journal cannot be reopened with the supplied identity");
        snapshot::Store snapshot_store(snapshot_path, identity, ledger);
        std::string snapshot_fingerprint;
        status = snapshot_store.read(identity, &report.pre_state, &snapshot_fingerprint);
        if (FAILED(status)) return ambiguous(status, "exact resource pre-state snapshot is unavailable");
        if (document.committed || !document.all_operations_succeeded || document.events.size() != 4 ||
            document.events[0].kind != journal::Event::Kind::intent ||
            document.events[0].intent.resource != "prestate" ||
            document.events[0].intent.action != "capture-install-or-repair" ||
            snapshot_fingerprint != document.events[0].intent.desired_fingerprint ||
            document.events[1].kind != journal::Event::Kind::observation ||
            document.events[1].observation.operation_status != S_OK ||
            document.events[1].observation.observed_fingerprint != document.events[0].intent.desired_fingerprint ||
            document.events[2].kind != journal::Event::Kind::intent ||
            document.events[2].intent.resource != "file-publication" ||
            document.events[2].intent.action != "apply" ||
            document.events[3].kind != journal::Event::Kind::observation ||
            document.events[3].observation.operation_status != S_OK ||
            document.events[3].observation.observed_fingerprint != document.events[2].intent.desired_fingerprint)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "journal is not exactly at the successful file-published, pre-TSF boundary");

        ResourceStates current_state{};
        status = detail::read_all(backend, current_state);
        if (status != S_OK) {
            report.classification = Classification::unreadable;
            report.status = status;
            report.evidence.push_back("one or more COM/TSF resources could not be read");
            return report;
        }
        if (current_state != report.pre_state)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "live COM/TSF state differs from the exact pre-file snapshot");

        status = files::FileDeployment::inspect(root, identity.owner, &report.current_files);
        if (FAILED(status)) return ambiguous(status, "published package files fail owner or hash validation");
        if ((journal::detail::machine_com_scope(identity) && report.current_files.has_x86) ||
            report.current_files.version != identity.package_version ||
            report.current_files.manifest_sha256 != identity.package_manifest_sha256)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "published package version or manifest differs from the transaction identity");
        std::string current_fingerprint;
        if (!durable_cli::detail::file_fingerprint(identity, true, &report.current_files,
                                                    nullptr, &current_fingerprint) ||
            current_fingerprint != document.events[3].observation.observed_fingerprint)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "published file snapshot differs from the durable file observation");

        std::vector<fs::path> file_journals;
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::none, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (it->path().filename().wstring().rfind(L".txn-", 0) == 0)
                file_journals.push_back(it->path());
        }
        if (ec || file_journals.size() != 1 ||
            file_journals.front().filename().wstring() != L".txn-" +
                std::wstring(identity.token.begin(), identity.token.end()) ||
            !recovery::known_journal_inventory(root, file_journals.front(), identity.owner))
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "file transaction journal does not match the durable owner/token identity");

        report.classification = Classification::file_published_tsf_unchanged;
        report.status = S_OK;
        report.evidence.push_back("file publication is observed and live COM/TSF still matches the exact pre-state");
        report.evidence.push_back("classification only; no automatic resume, rollback, or cleanup was performed");
        return report;
    } catch (...) {
        report.classification = Classification::unreadable;
        report.status = E_FAIL;
        report.evidence.push_back("unexpected read-only durable recovery inspection failure");
        return report;
    }
}

namespace detail {
inline bool full_state_fingerprint(const journal::Identity& identity,
                                   const ResourceStates& states,
                                   std::string* output) {
    if (!output) return false;
    std::string image;
    if (!snapshot::detail::encode(identity, states, &image)) return false;
    journal::detail::Digest digest{};
    if (!journal::detail::sha256(image, &digest)) return false;
    static constexpr char hex[] = "0123456789abcdef";
    output->clear();
    output->reserve(64);
    for (uint8_t byte : digest) {
        output->push_back(hex[byte >> 4]);
        output->push_back(hex[byte & 15]);
    }
    return true;
}

inline bool committed_singleton_state_contract(const journal::Identity& identity,
                                              const files::DeploymentSnapshot& published,
                                              const ResourceStates& prestate,
                                              const ResourceStates& current) {
    const std::wstring owner(identity.owner.begin(), identity.owner.end());
    const ResourceState& com64 = current[installer::detail::index(Resource::com_x64)];
    if (!com64.present || com64.owner != owner || com64.threading_model != L"Apartment" ||
        !installer::detail::same_path(com64.path, published.x64_dll.wstring()) ||
        current[installer::detail::index(Resource::com_x86)] !=
            prestate[installer::detail::index(Resource::com_x86)])
        return false;

    const auto& machine=current[installer::detail::index(Resource::machine_com_x64)];
    if (journal::detail::machine_com_scope(identity)) {
        if (!com64.created_by_lifecycle || !machine.present || !machine.created_by_lifecycle ||
            machine.owner!=owner || machine.threading_model!=L"Apartment" || machine.private_data.empty() ||
            !installer::detail::same_path(machine.path,published.x64_dll.wstring())) return false;
    } else if (machine!=ResourceState{}) return false;
    const ResourceState& profile = current[installer::detail::index(Resource::profile)];
    const ResourceState& keyboard = current[installer::detail::index(Resource::keyboard_category)];
    const ResourceState& tray = current[installer::detail::index(Resource::system_tray_category)];
    const ResourceState& ui_element = current[installer::detail::index(Resource::ui_element_category)];
    if (!profile.present || !keyboard.present || !tray.present || !ui_element.present ||
        profile.owner != owner || keyboard.owner != owner || tray.owner != owner || ui_element.owner != owner ||
        (profile.created_by_lifecycle &&
         !installer::detail::same_path(profile.path, published.branding_dll.wstring())) ||
        (keyboard.created_by_lifecycle && keyboard.owner != owner) ||
        (tray.created_by_lifecycle && tray.owner != owner) ||
        (ui_element.created_by_lifecycle && ui_element.owner != owner))
        return false;
    return true;
}

inline bool committed_singleton_events_valid(const journal::Document& document,
                                             const std::string& prestate_fingerprint, bool machine_scope = false) {
    using Kind = journal::Event::Kind;
    if (!document.committed || !document.all_operations_succeeded ||
        document.events.size() < 5 || document.events.back().kind != Kind::commit ||
        document.events.front().kind != Kind::intent ||
        document.events.front().intent.resource != "prestate" ||
        document.events.front().intent.action != "capture-install-or-repair" ||
        document.events.front().intent.expected_before_fingerprint != std::string(64, '0') ||
        document.events.front().intent.desired_fingerprint != prestate_fingerprint ||
        document.events[1].kind != Kind::observation ||
        document.events[1].observation.sequence != document.events.front().intent.sequence ||
        document.events[1].observation.operation_status != S_OK ||
        document.events[1].observation.observed_fingerprint != prestate_fingerprint ||
        document.events[2].kind != Kind::intent ||
        document.events[2].intent.resource != "file-publication" ||
        document.events[2].intent.action != "apply" ||
        document.events[3].kind != Kind::observation ||
        document.events[3].observation.sequence != document.events[2].intent.sequence ||
        document.events[3].observation.operation_status != S_OK ||
        document.events[3].observation.observed_fingerprint != document.events[2].intent.desired_fingerprint)
        return false;

    for (size_t i = 4; i + 1 < document.events.size() - 1; i += 2) {
        const auto& intent = document.events[i];
        const auto& observation = document.events[i + 1];
        if (intent.kind != Kind::intent || observation.kind != Kind::observation ||
            observation.observation.sequence != intent.intent.sequence ||
            intent.intent.action != "apply" ||
            intent.intent.resource == "com-x86" ||
            (intent.intent.resource != "com-x64" &&
             !(machine_scope && intent.intent.resource == "com-machine-x64") && intent.intent.resource != "profile" &&
             intent.intent.resource != "keyboard-category" &&
             intent.intent.resource != "system-tray-category" &&
             intent.intent.resource != "ui-element-category") ||
            observation.observation.operation_status != S_OK ||
            observation.observation.observed_fingerprint != intent.intent.desired_fingerprint)
            return false;
    }
    return (document.events.size() - 5) % 2 == 0 &&
           document.events.back().commit_fingerprint.size() == 64;
}
// Schema 3 also checks every resource intent against independently projected
// pre/final values. A final full-state hash alone cannot explain an unknown or
// omitted machine mutation.
inline bool committed_machine_resources_valid(const journal::Identity& identity,
        const journal::Document& document,const ResourceStates& before,
        const ResourceStates& current,Backend& backend) {
    std::array<bool,static_cast<size_t>(Resource::count)> seen{};
    for (size_t at=4;at+1<document.events.size()-1;at+=2) {
        const auto& intent=document.events[at].intent;
        size_t index=static_cast<size_t>(Resource::count);
        for (size_t i=0;i<snapshot::count_for(identity);++i)
            if (intent.resource==durable_observer::detail::resource_name(static_cast<Resource>(i))) {index=i;break;}
        if (index==static_cast<size_t>(Resource::count) || seen[index]) return false;
        seen[index]=true;
        const auto resource=static_cast<Resource>(index);
        ResourceState projected_before=before[index],projected_current=current[index];
        // Observer projects the expected-before state only for machine COM.
        if (resource==Resource::machine_com_x64 &&
            backend.project_apply_state(resource,before[index],&projected_before)!=S_OK) return false;
        if (backend.project_apply_state(resource,current[index],&projected_current)!=S_OK) return false;
        std::string old_hash,new_hash;
        if (!durable_observer::detail::fingerprint_state(identity,resource,projected_before,&old_hash,false) ||
            !durable_observer::detail::fingerprint_state(identity,resource,projected_current,&new_hash,false) ||
            intent.expected_before_fingerprint!=old_hash || intent.desired_fingerprint!=new_hash) return false;
    }
    for (size_t i=0;i<seen.size();++i) if (!seen[i] && before[i]!=current[i]) return false;
    return true;
}
} // namespace detail

// Read-only classification for a fully committed singleton x64 package.
// COM32 is accepted only when its complete live ResourceState is identical to
// the durable prestate snapshot; it is never inferred to be owned by this package.
inline Report inspect_committed_x64_singleton(const journal::Identity& identity,
                                              Backend& backend) {
    Report report;
    auto ambiguous = [&](HRESULT status, const char* reason) {
        report.classification = Classification::incomplete_or_ambiguous;
        report.status = status;
        report.evidence.push_back(reason);
        return report;
    };
    try {
        if (!journal::detail::valid_identity(identity) || (identity.schema_version != 2 && identity.schema_version != 3)) {
            report.classification = Classification::unreadable;
            report.status = E_INVALIDARG;
            report.evidence.push_back("durable transaction identity is invalid");
            return report;
        }
        const fs::path root(identity.install_root);
        const fs::path ledger(identity.storage_root);
        journal::Store journal_store(ledger / L"transaction.journal", identity, ledger);
        journal::Document document;
        HRESULT status = journal_store.read(identity, &document);
        if (status != S_OK) return ambiguous(status, "journal cannot be reopened with the supplied identity and exact S_OK status");

        snapshot::Store snapshot_store(ledger / L"transaction.snapshot", identity, ledger);
        std::string prestate_fingerprint;
        status = snapshot_store.read(identity, &report.pre_state, &prestate_fingerprint);
        if (status != S_OK) return ambiguous(status, "exact resource pre-state snapshot is unavailable with exact S_OK status");
        if (!detail::committed_singleton_events_valid(document, prestate_fingerprint,journal::detail::machine_com_scope(identity)))
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "journal is not a complete successful x64-only install/repair commit");

        status = files::FileDeployment::inspect(root, identity.owner, &report.current_files);
        if (status != S_OK) return ambiguous(status, "published package files fail owner or hash validation or exact S_OK status");
        if (report.current_files.has_x86 || report.current_files.version != identity.package_version ||
            report.current_files.manifest_sha256 != identity.package_manifest_sha256)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "installed package is not the exact x64-only manifest bound to the transaction");
        std::string file_fingerprint;
        if (!durable_cli::detail::file_fingerprint(identity, true, &report.current_files,
                                                    nullptr, &file_fingerprint) ||
            file_fingerprint != document.events[3].observation.observed_fingerprint)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "installed file state differs from the durable publication observation");

        std::vector<fs::path> file_journals;
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::none, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (it->path().filename().wstring().rfind(L".txn-", 0) == 0)
                file_journals.push_back(it->path());
        }
        if (ec || file_journals.size() > 1 ||
            (file_journals.size() == 1 &&
             (file_journals.front().filename().wstring() != L".txn-" +
                  std::wstring(identity.token.begin(), identity.token.end()) ||
              !recovery::known_journal_inventory(root, file_journals.front(), identity.owner))))
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "file transaction artifacts are duplicated or do not match the durable owner/token");

        ResourceStates current_state{};
        for (size_t i = 0; i < current_state.size(); ++i) {
            status = backend.read(static_cast<Resource>(i), &current_state[i]);
            if (status != S_OK) {
                report.classification = Classification::unreadable;
                report.status = status;
                report.evidence.push_back("one or more COM/TSF resources did not return exact S_OK");
                return report;
            }
        }
        if (!detail::committed_singleton_state_contract(identity, report.current_files,
                                                        report.pre_state, current_state))
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "live COM64/shared state violates the package contract or COM32 differs from prestate");
        if (journal::detail::machine_com_scope(identity) &&
            !detail::committed_machine_resources_valid(identity,document,report.pre_state,current_state,backend))
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "machine resource apply fingerprints or untouched resource states differ from the durable contract");
        std::string current_fingerprint;
        if (!detail::full_state_fingerprint(identity, current_state, &current_fingerprint) ||
            current_fingerprint != document.events.back().commit_fingerprint)
            return ambiguous(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH),
                "live COM/TSF state differs from the exact durable commit fingerprint");
        status = backend.verify_install_health();
        if (status != S_OK)
            return ambiguous(status, "read-only install health did not return exact S_OK");

        report.classification = Classification::committed_x64_singleton_coherent;
        report.status = S_OK;
        report.evidence.push_back("x64-only package, token-bound journal, exact prestate, and live commit fingerprint agree");
        report.evidence.push_back("COM32 is byte-for-byte unchanged from prestate; classification only, no automatic action");
        return report;
    } catch (...) {
        report.classification = Classification::unreadable;
        report.status = E_FAIL;
        report.evidence.push_back("unexpected read-only committed singleton inspection failure");
        return report;
    }
}
} // namespace boshiamy::installer::durable_recovery
