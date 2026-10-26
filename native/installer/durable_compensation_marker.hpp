#pragma once

#include "durable_transaction_observer.hpp"
#include "file_deployment.hpp"

namespace boshiamy::installer::durable_compensation_marker {
namespace fs = std::filesystem;
namespace journal = durable_journal;
namespace snapshot = durable_snapshot;

// The same file-phase fingerprint used by Session. Keeping it independent of
// Session lets history validate a terminal record without including the CLI.
inline bool file_fingerprint(const journal::Identity& identity, bool present,
                             const files::DeploymentSnapshot* snapshot_value,
                             const files::DeploymentPaths* desired_value,
                             std::string* output) {
    if (!output || (present && ((!snapshot_value && !desired_value) ||
        (snapshot_value && desired_value)))) return false;
    const auto field = [](std::string* image, const std::string& value) {
        if (value.size() > (std::numeric_limits<uint32_t>::max)()) return false;
        journal::detail::put_u32(image, static_cast<uint32_t>(value.size()));
        image->append(value); return true;
    };
    const auto path_field = [&](std::string* image, const fs::path& path) {
        std::string value;
        return journal::detail::utf8(path.lexically_normal().wstring(), &value) && field(image,value);
    };
    std::string image("LiuKai-FilePhase-v1",19);
    image.push_back(present ? 1 : 0);
    std::string install_root,storage_root;
    if (!field(&image,identity.owner) || !field(&image,identity.token) ||
        !field(&image,identity.package_version) || !field(&image,identity.package_manifest_sha256) ||
        !journal::detail::utf8(identity.install_root,&install_root) ||
        !journal::detail::utf8(identity.storage_root,&storage_root) ||
        !field(&image,install_root) || !field(&image,storage_root)) return false;
    if (present) {
        const auto& version=snapshot_value?snapshot_value->version:desired_value->version;
        const auto& manifest=snapshot_value?snapshot_value->manifest_sha256:desired_value->manifest_sha256;
        const auto& cli=snapshot_value?snapshot_value->installer_cli_sha256:desired_value->installer_cli_sha256;
        const auto& branding=snapshot_value?snapshot_value->branding_sha256:desired_value->branding_sha256;
        const auto& x64=snapshot_value?snapshot_value->x64_dll:desired_value->x64_dll;
        const auto& x86=snapshot_value?snapshot_value->x86_dll:desired_value->x86_dll;
        const auto& icon=snapshot_value?snapshot_value->branding_dll:desired_value->branding_dll;
        const auto& cli_path=snapshot_value?snapshot_value->installer_cli:desired_value->installer_cli;
        const bool has_x86=snapshot_value?snapshot_value->has_x86:desired_value->has_x86;
        if (has_x86 != !x86.empty() || !field(&image,version) || !field(&image,manifest) ||
            !field(&image,cli) || !field(&image,branding) || !path_field(&image,x64) ||
            !path_field(&image,x86) || !path_field(&image,icon) ||
            !path_field(&image,cli_path)) return false;
    }
    *output=files::detail::sha256_bytes(image);
    return files::detail::valid_hash(*output);
}

struct Facts {
    ResourceStates prestate{};
    std::string prestate_fingerprint;
    std::string forward_hash;
    std::string snapshot_hash;
    std::string old_file_fingerprint;
    uint64_t rollback_sequence=0;
};

using ApplyProjection = durable_observer::ApplyProjection;

// A terminal proof is valid only after every known resource apply has a closed
// readback and every changed resource has an exact compensating restore.
inline bool resource_compensation_valid(const journal::Identity& identity,
        const journal::Document& document, const ResourceStates& prestate,
        size_t begin, size_t end, const ApplyProjection& projection) {
    if (begin > end || (end-begin)%2) return false;
    std::array<bool,static_cast<size_t>(Resource::count)> applied{}, restored{}, changed{};
    std::array<std::string,static_cast<size_t>(Resource::count)> readback{};
    bool restoring=false,failed=false;
    for (size_t at=begin; at<end; at+=2) {
        const auto& intent_event=document.events[at];
        const auto& observed_event=document.events[at+1];
        if (intent_event.kind!=journal::Event::Kind::intent ||
            observed_event.kind!=journal::Event::Kind::observation ||
            intent_event.intent.sequence==0 ||
            observed_event.observation.sequence!=intent_event.intent.sequence ||
            observed_event.observation.observed_fingerprint==durable_observer::detail::unknown_fingerprint()) return false;
        size_t index=static_cast<size_t>(Resource::count);
        for (size_t i=0;i<snapshot::count_for(identity);++i)
            if (intent_event.intent.resource==durable_observer::detail::resource_name(static_cast<Resource>(i))) {index=i;break;}
        if (index==static_cast<size_t>(Resource::count)) return false;
        const auto resource=static_cast<Resource>(index);
        if (journal::detail::machine_com_scope(identity) && resource==Resource::com_x86) return false;
        const bool apply=intent_event.intent.action=="apply",restore=intent_event.intent.action=="restore";
        if ((!apply&&!restore) || (apply&&(restoring||failed||applied[index])) ||
            (restore&&(!applied[index]||restored[index]))) return false;
        ResourceState original=prestate[index];
        if (apply && resource==Resource::machine_com_x64) {
            if (projection) {
                ResourceState projected;
                if (projection(resource,original,&projected)!=S_OK) return false;
                original=std::move(projected);
            } else if (original.present) return false;
        }
        std::string original_fingerprint;
        if (!durable_observer::detail::fingerprint_state(identity,resource,original,&original_fingerprint,restore)) return false;
        if (apply) {
            if (intent_event.intent.expected_before_fingerprint!=original_fingerprint ||
                !journal::detail::valid_hash(intent_event.intent.desired_fingerprint)) return false;
            applied[index]=true;
            const auto& observation=observed_event.observation;
            if ((observation.operation_status==S_OK && observation.observed_fingerprint!=intent_event.intent.desired_fingerprint) ||
                (observation.operation_status!=S_OK && observation.observed_fingerprint!=original_fingerprint &&
                 !observation.mutation_may_have_occurred)) return false;
            readback[index]=observation.observed_fingerprint;
            changed[index]=readback[index]!=original_fingerprint;
            failed=observation.operation_status!=S_OK;
        } else {
            restoring=true;
            // Every COM apply uses an owned semantic projection, while restore
            // fingerprints include the raw tree. Their domains differ.
            const bool com=resource==Resource::com_x64 || resource==Resource::com_x86 || resource==Resource::machine_com_x64;
            if ((com ? !journal::detail::valid_hash(intent_event.intent.expected_before_fingerprint) :
                 intent_event.intent.expected_before_fingerprint!=readback[index]) ||
                intent_event.intent.desired_fingerprint!=original_fingerprint ||
                observed_event.observation.operation_status!=S_OK ||
                observed_event.observation.observed_fingerprint!=original_fingerprint) return false;
            restored[index]=true;
        }
    }
    for (size_t i=0;i<applied.size();++i) if (changed[i]&&!restored[i]) return false;
    return true;
}

inline HRESULT collect(const journal::Identity& identity, const fs::path& requested_root, Facts* out,
                       const ApplyProjection& projection = {}, bool require_current_files = true) {
    if (!out) return E_POINTER;
    *out={};
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    try {
        if ((identity.schema_version!=2 && identity.schema_version!=3) || identity.owner!="LiuKai-TSF-v1" ||
            !journal::detail::valid_identity(identity) ||
            !journal::detail::same_path(identity.install_root,requested_root.wstring())) return conflict;
        const fs::path ledger=identity.storage_root;
        const fs::path journal_path=ledger/L"transaction.journal";
        const fs::path snapshot_path=ledger/L"transaction.snapshot";
        if (!files::detail::absolute_no_reparse(ledger,true) ||
            !files::detail::absolute_no_reparse(journal_path,true) ||
            !files::detail::absolute_no_reparse(snapshot_path,true)) return conflict;
        journal::Store journal_store(journal_path,identity,ledger);
        journal::Document document;
        HRESULT status=journal_store.read(identity,&document);
        if (status!=S_OK || document.committed || document.events.size()<6) return conflict;
        snapshot::Store snapshot_store(snapshot_path,identity,ledger);
        status=snapshot_store.read(identity,&out->prestate,&out->prestate_fingerprint);
        if (status!=S_OK) return conflict;
        using Kind=journal::Event::Kind;
        const auto& first=document.events[0];
        const auto& first_observed=document.events[1];
        const auto& apply=document.events[2];
        const auto& applied=document.events[3];
        const auto& restore=document.events[document.events.size()-2];
        const auto& restored=document.events.back();
        if (first.kind!=Kind::intent || first.intent.resource!="prestate" ||
            first.intent.action!="capture-install-or-repair" ||
            first.intent.expected_before_fingerprint!=std::string(64,'0') ||
            first.intent.desired_fingerprint!=out->prestate_fingerprint ||
            first_observed.kind!=Kind::observation ||
            first_observed.observation.sequence!=first.intent.sequence ||
            first_observed.observation.operation_status!=S_OK ||
            first_observed.observation.observed_fingerprint!=out->prestate_fingerprint ||
            apply.kind!=Kind::intent || apply.intent.resource!="file-publication" ||
            apply.intent.action!="apply" || applied.kind!=Kind::observation ||
            applied.observation.sequence!=apply.intent.sequence ||
            applied.observation.operation_status!=S_OK ||
            applied.observation.observed_fingerprint!=apply.intent.desired_fingerprint ||
            restore.kind!=Kind::intent || restore.intent.resource!="file-publication" ||
            restore.intent.action!="rollback" ||
            restore.intent.expected_before_fingerprint!=apply.intent.desired_fingerprint ||
            restore.intent.desired_fingerprint!=apply.intent.expected_before_fingerprint ||
            restored.kind!=Kind::observation || restored.observation.sequence!=restore.intent.sequence ||
            restored.observation.operation_status!=S_OK ||
            restored.observation.observed_fingerprint!=restore.intent.desired_fingerprint) return conflict;
        if (!resource_compensation_valid(identity,document,out->prestate,4,document.events.size()-2,projection)) return conflict;
        out->old_file_fingerprint=restore.intent.desired_fingerprint;
        out->rollback_sequence=restore.intent.sequence;
        // Minting always verifies the current restored files. Schema-3 history
        // inspection instead validates the frozen terminal proof: a later
        // committed upgrade may legitimately advance the installed files.
        if (require_current_files) {
            std::error_code error;
            const bool present=fs::exists(requested_root,error);
            if (error) return conflict;
            files::DeploymentSnapshot current;
            const files::DeploymentSnapshot* current_ptr=nullptr;
            if (present) {
                if (files::FileDeployment::inspect(requested_root,identity.owner,&current)!=S_OK) return conflict;
                current_ptr=&current;
            }
            std::string observed_file_fingerprint;
            if (!file_fingerprint(identity,present,current_ptr,nullptr,&observed_file_fingerprint) ||
                observed_file_fingerprint!=out->old_file_fingerprint) return conflict;
        }
        out->forward_hash=files::detail::sha256_file(journal_path);
        out->snapshot_hash=files::detail::sha256_file(snapshot_path);
        if (!files::detail::valid_hash(out->forward_hash) ||
            !files::detail::valid_hash(out->snapshot_hash)) return conflict;
        return S_OK;
    } catch (...) { return conflict; }
}

inline HRESULT inspect(const journal::Identity& identity, const fs::path& requested_root,
                       const ApplyProjection& projection = {}) {
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    Facts facts;
    if (collect(identity,requested_root,&facts,projection,!journal::detail::machine_com_scope(identity))!=S_OK) return conflict;
    try {
        const fs::path ledger=identity.storage_root;
        journal::Store marker(ledger/L"terminal-compensation.journal",identity,ledger);
        journal::Document document;
        if (marker.read(identity,&document)!=S_OK || !document.committed ||
            !document.all_operations_succeeded || document.events.size()!=5) return conflict;
        using Kind=journal::Event::Kind;
        const auto& bind=document.events[0].intent;
        const auto& bind_observed=document.events[1].observation;
        const auto& proof=document.events[2].intent;
        const auto& proof_observed=document.events[3].observation;
        if (document.events[0].kind!=Kind::intent || document.events[1].kind!=Kind::observation ||
            document.events[2].kind!=Kind::intent || document.events[3].kind!=Kind::observation ||
            document.events[4].kind!=Kind::commit ||
            bind.sequence!=1 || bind.resource!="failed-deployment" || bind.action!="terminal-compensation" ||
            bind.expected_before_fingerprint!=facts.forward_hash ||
            bind.desired_fingerprint!=facts.prestate_fingerprint ||
            bind_observed.sequence!=1 || bind_observed.operation_status!=S_OK ||
            bind_observed.observed_fingerprint!=facts.prestate_fingerprint ||
            bind_observed.mutation_may_have_occurred ||
            proof.sequence!=2 || proof.resource!="file-rollback" ||
            proof.action!="observation-"+std::to_string(facts.rollback_sequence) ||
            proof.expected_before_fingerprint!=facts.snapshot_hash ||
            proof.desired_fingerprint!=facts.old_file_fingerprint ||
            proof_observed.sequence!=2 || proof_observed.operation_status!=S_OK ||
            proof_observed.observed_fingerprint!=facts.old_file_fingerprint ||
            proof_observed.mutation_may_have_occurred ||
            document.events[4].commit_fingerprint!=facts.prestate_fingerprint) return conflict;
        return S_OK;
    } catch (...) { return conflict; }
}

inline HRESULT record(const journal::Identity& identity, const ResourceStates& restored,
                      const std::string& expected_old_file_fingerprint,
                      const ApplyProjection& projection = {}) {
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    Facts facts;
    if (collect(identity,fs::path(identity.install_root),&facts,projection)!=S_OK ||
        restored!=facts.prestate || expected_old_file_fingerprint!=facts.old_file_fingerprint) return conflict;
    try {
        const fs::path ledger=identity.storage_root;
        journal::Store marker(ledger/L"terminal-compensation.journal",identity,ledger);
        HRESULT status=marker.create();
        if (status!=S_OK) return status;
        uint64_t sequence=0;
        status=marker.append_intent("failed-deployment","terminal-compensation",
            facts.forward_hash,facts.prestate_fingerprint,&sequence);
        if (status!=S_OK) return status;
        status=marker.append_observation(sequence,S_OK,facts.prestate_fingerprint,false);
        if (status!=S_OK) return status;
        status=marker.append_intent("file-rollback",
            "observation-"+std::to_string(facts.rollback_sequence),
            facts.snapshot_hash,facts.old_file_fingerprint,&sequence);
        if (status!=S_OK) return status;
        status=marker.append_observation(sequence,S_OK,facts.old_file_fingerprint,false);
        if (status!=S_OK) return status;
        status=marker.append_commit(facts.prestate_fingerprint);
        return status==S_OK ? inspect(identity,fs::path(identity.install_root),projection) : status;
    } catch (...) { return conflict; }
}
// Registration-only uninstall never changes package files. Its terminal
// compensation proof is separate from file rollback and cannot be interchanged.
inline HRESULT collect_registration(const journal::Identity& identity,const fs::path& root,
        Facts* facts,const ApplyProjection& projection = {}) {
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (!facts) return E_POINTER;
    *facts={};
    try {
        if (!journal::detail::valid_identity(identity) || !journal::detail::machine_com_scope(identity) ||
            identity.owner!="LiuKai-TSF-v1" || !journal::detail::same_path(identity.install_root,root.wstring())) return conflict;
        const fs::path ledger(identity.storage_root),forward=ledger/L"transaction.journal",saved=ledger/L"transaction.snapshot";
        if (!files::detail::absolute_no_reparse(ledger,true) ||
            !files::detail::absolute_no_reparse(forward,true) ||
            !files::detail::absolute_no_reparse(saved,true)) return conflict;
        journal::Store store(forward,identity,ledger);
        journal::Document document;
        snapshot::Store snapshot_store(saved,identity,ledger);
        if (store.read(identity,&document)!=S_OK || document.committed || document.events.size()<2 ||
            snapshot_store.read(identity,&facts->prestate,&facts->prestate_fingerprint)!=S_OK) return conflict;
        const auto& intent=document.events[0];const auto& observation=document.events[1];
        if (intent.kind!=journal::Event::Kind::intent || intent.intent.resource!="prestate" ||
            intent.intent.action!="capture-uninstall" || intent.intent.expected_before_fingerprint!=std::string(64,'0') ||
            intent.intent.desired_fingerprint!=facts->prestate_fingerprint || observation.kind!=journal::Event::Kind::observation ||
            observation.observation.sequence!=intent.intent.sequence || observation.observation.operation_status!=S_OK ||
            observation.observation.observed_fingerprint!=facts->prestate_fingerprint ||
            !resource_compensation_valid(identity,document,facts->prestate,2,document.events.size(),projection)) return conflict;
        facts->forward_hash=files::detail::sha256_file(forward);
        facts->snapshot_hash=files::detail::sha256_file(saved);
        return files::detail::valid_hash(facts->forward_hash)&&files::detail::valid_hash(facts->snapshot_hash)?S_OK:conflict;
    } catch (...) {return conflict;}
}
inline HRESULT inspect_registration(const journal::Identity& identity,const fs::path& root,
        const ApplyProjection& projection = {}) {
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    Facts facts;if (collect_registration(identity,root,&facts,projection)!=S_OK) return conflict;
    try {
        const fs::path ledger(identity.storage_root);
        journal::Store store(ledger/L"terminal-registration-compensation.journal",identity,ledger);
        journal::Document document;
        if (store.read(identity,&document)!=S_OK || !document.committed || !document.all_operations_succeeded ||
            document.events.size()!=5) return conflict;
        const auto& bind=document.events[0];const auto& bound=document.events[1];
        const auto& proof=document.events[2];const auto& proved=document.events[3];
        if (bind.kind!=journal::Event::Kind::intent || bind.intent.sequence!=1 ||
            bind.intent.resource!="failed-registration" || bind.intent.action!="terminal-compensation" ||
            bind.intent.expected_before_fingerprint!=facts.forward_hash || bind.intent.desired_fingerprint!=facts.prestate_fingerprint ||
            bound.kind!=journal::Event::Kind::observation || bound.observation.sequence!=1 ||
            bound.observation.operation_status!=S_OK || bound.observation.mutation_may_have_occurred ||
            bound.observation.observed_fingerprint!=facts.prestate_fingerprint ||
            proof.kind!=journal::Event::Kind::intent || proof.intent.sequence!=2 ||
            proof.intent.resource!="snapshot" || proof.intent.action!="exact-restoration" ||
            proof.intent.expected_before_fingerprint!=facts.snapshot_hash || proof.intent.desired_fingerprint!=facts.prestate_fingerprint ||
            proved.kind!=journal::Event::Kind::observation || proved.observation.sequence!=2 ||
            proved.observation.operation_status!=S_OK || proved.observation.mutation_may_have_occurred ||
            proved.observation.observed_fingerprint!=facts.prestate_fingerprint || document.events[4].kind!=journal::Event::Kind::commit ||
            document.events[4].commit_fingerprint!=facts.prestate_fingerprint) return conflict;
        return S_OK;
    } catch (...) {return conflict;}
}
inline HRESULT record_registration(const journal::Identity& identity,const ResourceStates& restored,
        const ApplyProjection& projection = {}) {
    const HRESULT conflict=HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    Facts facts;if (collect_registration(identity,fs::path(identity.install_root),&facts,projection)!=S_OK ||
        restored!=facts.prestate) return conflict;
    try {
        const fs::path ledger(identity.storage_root);
        journal::Store marker(ledger/L"terminal-registration-compensation.journal",identity,ledger);
        HRESULT status=marker.create();if(status!=S_OK)return status;
        uint64_t sequence=0;
        status=marker.append_intent("failed-registration","terminal-compensation",facts.forward_hash,facts.prestate_fingerprint,&sequence);
        if(status!=S_OK)return status;
        status=marker.append_observation(sequence,S_OK,facts.prestate_fingerprint,false);if(status!=S_OK)return status;
        status=marker.append_intent("snapshot","exact-restoration",facts.snapshot_hash,facts.prestate_fingerprint,&sequence);
        if(status!=S_OK)return status;
        status=marker.append_observation(sequence,S_OK,facts.prestate_fingerprint,false);if(status!=S_OK)return status;
        status=marker.append_commit(facts.prestate_fingerprint);
        return status==S_OK?inspect_registration(identity,fs::path(identity.install_root),projection):status;
    } catch (...) {return conflict;}
}
} // namespace boshiamy::installer::durable_compensation_marker
