#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <wix-binding.hpp>
#include "fresh-registration.hpp"
#include "fresh-root.hpp"
#include "retired-files.hpp"
#include "modern-categories.hpp"
#include "failure-collector.hpp"
#include "early-failure-collector.hpp"
#include "legacy-recovery-runner.hpp"
#include "../installer/tsf_shared_store.hpp"
#include "../installer/durable_cli_session.hpp"
#include "../installer/dev_host_migration_history.hpp"

bool elevated_process_user(std::wstring& sid);
bool property(MSIHANDLE install,const wchar_t* name,std::wstring& value);
using namespace boshiamy::installer;

static void collect_early_failure(MSIHANDLE install,const wchar_t* stage,HRESULT failure) {
    // Immediate-action failures happen before InstallFiles, so the rollback
    // collector cannot read an installed script. Use only the hash-pinned MSI
    // Binary stream, a fresh Program Files staging directory and the verified
    // interactive account. Never alter or hide the original MSI error.
    if(MsiGetMode(install,MSIRUNMODE_SCHEDULED)||MsiGetMode(install,MSIRUNMODE_ROLLBACK)||MsiGetMode(install,MSIRUNMODE_COMMIT))return;
    std::wstring sid,log;if(!elevated_process_user(sid))return;
    if(!property(install,L"MsiLogFileLocation",log)||(!log.empty()&&!std::filesystem::path(log).is_absolute()))log.clear();
    PWSTR programs=nullptr,local=nullptr;
    if(SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programs)!=S_OK)return;
    const auto parent=std::filesystem::path(programs).lexically_normal();CoTaskMemFree(programs);
    if(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local)!=S_OK)return;
    const auto output=(std::filesystem::path(local)/L"LiuKai"/L"diagnostics").lexically_normal();CoTaskMemFree(local);
    MSIHANDLE database=MsiGetActiveDatabase(install);if(!database)return;
    DWORD result=ERROR_GEN_FAILURE;
    try {result=liukai::wix_diagnostics::run_binary(database,parent,parent/L"LiuKai",output,log,kWixDiagnosticsSha256,stage,std::to_wstring(static_cast<unsigned long>(failure)));}catch(...){}
    MsiCloseHandle(database);
    MSIHANDLE record=MsiCreateRecord(1);if(record){const auto message=std::wstring(L"LiuKai early diagnostics exit=")+std::to_wstring(result);MsiRecordSetStringW(record,0,message.c_str());MsiProcessMessage(install,INSTALLMESSAGE_INFO,record);MsiCloseHandle(record);}
}
static UINT failed(MSIHANDLE install,const wchar_t* stage,HRESULT result) {
    const auto message=std::wstring(L"LiuKai registration: ")+stage+L" HRESULT="+std::to_wstring(static_cast<unsigned long>(result));
    MSIHANDLE record=MsiCreateRecord(1);
    if(record){MsiRecordSetStringW(record,0,message.c_str());MsiProcessMessage(install,INSTALLMESSAGE_ERROR,record);MsiCloseHandle(record);}
    try {collect_early_failure(install,stage,result);} catch(...) {}
    return ERROR_INSTALL_FAILURE;
}
static void trace_stage(MSIHANDLE install,const wchar_t* stage) {
    MSIHANDLE record=MsiCreateRecord(1);if(!record)return;
    const std::wstring message=std::wstring(L"LiuKai registration stage: ")+stage;
    if(MsiRecordSetStringW(record,0,message.c_str())==ERROR_SUCCESS)MsiProcessMessage(install,INSTALLMESSAGE_INFO,record);
    MsiCloseHandle(record);
}
struct StageOnDestruction {
    MSIHANDLE install;const wchar_t* stage;
    ~StageOnDestruction(){trace_stage(install,stage);}
};
static bool prepare_failure_collection(MSIHANDLE install,const wchar_t* name,const std::wstring& sid,const std::wstring& root) {
    std::wstring log;if(!property(install,L"MsiLogFileLocation",log))log.clear();
    if(sid.find(L'|')!=std::wstring::npos||root.find(L'|')!=std::wstring::npos||log.find(L'|')!=std::wstring::npos)return false;
    const auto data=sid+L"|"+root+L"|"+log;
    return MsiSetPropertyW(install,name,data.c_str())==ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) UINT __stdcall CollectFailure(MSIHANDLE install) {
    try {
        std::wstring data,sid,expected,root,log;
        if(!property(install,L"CustomActionData",data)||!liukai::wix_diagnostics::parse_data(data,expected,root,log)||
           !elevated_process_user(sid)||sid!=expected){trace_stage(install,L"diagnostic-context-rejected");return ERROR_SUCCESS;}
        PWSTR programs=nullptr,local=nullptr;
        if(SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programs)!=S_OK)return ERROR_SUCCESS;
        const auto canonical=(std::filesystem::path(programs)/L"LiuKai").lexically_normal();CoTaskMemFree(programs);
        if(!detail::same_path(canonical.wstring(),msi_registration::normalized_root(root).wstring())){trace_stage(install,L"diagnostic-root-rejected");return ERROR_SUCCESS;}
        if(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local)!=S_OK)return ERROR_SUCCESS;
        const auto output=(std::filesystem::path(local)/L"LiuKai"/L"diagnostics").lexically_normal();CoTaskMemFree(local);
        if(!log.empty()&&!std::filesystem::path(log).is_absolute())log.clear();
        const auto result=liukai::wix_diagnostics::run(canonical,output,log,kWixDiagnosticsSha256);
        trace_stage(install,(std::wstring(L"diagnostic-exit-")+std::to_wstring(result)).c_str());
    } catch(...) {trace_stage(install,L"diagnostic-exception");}
    return ERROR_SUCCESS;
}
static bool verified_helper(const std::filesystem::path& root,std::wstring* output) {
    if(!output)return false;
    const auto helper=root/L"LiuKaiInstaller.exe";
    if(!files::detail::absolute_no_reparse(helper,true)||files::detail::sha256_file(helper)!=kWixHelperSha256)return false;
    *output=helper.wstring();return true;
}

static durable_journal::Identity modern_identity(const std::filesystem::path& root,const std::string& token,const std::string& version) {
    durable_journal::Identity id;id.owner="LiuKai-TSF-v1";id.token=token;id.package_version=version;
    id.package_manifest_sha256=kWixManifestSha256;
    id.install_root=(root.parent_path()/L".liukai-msi-modern"/L"LiuKai-TSF-v1"/std::filesystem::path(token)).wstring();
    return id;
}
static HRESULT enable_modern_categories(const std::filesystem::path& root,const std::string& token,const std::string& version) {
    auto id=modern_identity(root,token,version);const std::filesystem::path dir(id.install_root);
    if(!durable_cli::detail::ensure_directory_chain(dir.parent_path()))return E_ACCESSDENIED;
    if(!CreateDirectoryW(dir.c_str(),nullptr))return HRESULT_FROM_WIN32(GetLastError());
    package_categories::WindowsStore categories;package_categories::Categories initial;
    HRESULT hr=package_categories::capture(categories,&initial);if(hr!=S_OK)return hr;
    durable_journal::Store journal(dir/L"add.journal",id);msi_modern_categories::DurableObserver observer(journal,id);
    return msi_modern_categories::transition(categories,initial,true,observer);
}
static HRESULT disable_modern_categories(const std::filesystem::path& root,const std::string& token,
        const std::string& version,const std::string& undo,bool allowMissing) {
    auto id=modern_identity(root,token,version);const std::filesystem::path dir(id.install_root);
    const DWORD attrs=GetFileAttributesW(dir.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();return allowMissing&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)?S_OK:HRESULT_FROM_WIN32(error);}
    if(!files::detail::absolute_no_reparse(dir,true))return E_ACCESSDENIED;
    const auto addPath=dir/L"add.journal";const DWORD addAttrs=GetFileAttributesW(addPath.c_str());
    if(addAttrs==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();return allowMissing&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)?S_OK:HRESULT_FROM_WIN32(error);}
    package_categories::WindowsStore categories;durable_journal::Store forward(addPath,id);durable_journal::Document doc;
    HRESULT hr=forward.read(id,&doc);if(hr!=S_OK)return hr;
    durable_journal::Store reverse(dir/(L"remove-"+std::wstring(undo.begin(),undo.end())+L".journal"),id);
    msi_modern_categories::DurableObserver observer(reverse,id);
    if(!doc.committed)return allowMissing?msi_modern_categories::recover_partial(categories,forward,id,true,observer):HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    hr=msi_modern_categories::inspect_committed(categories,forward,id,true);if(hr!=S_OK)return hr;
    package_categories::Categories current;hr=package_categories::capture(categories,&current);if(hr!=S_OK)return hr;
    return msi_modern_categories::transition(categories,current,false,observer);
}
static HRESULT restore_modern_categories(const std::filesystem::path& root,const std::string& token,
        const std::string& version,const std::string& removed,const std::string& restore) {
    auto id=modern_identity(root,token,version);const std::filesystem::path dir(id.install_root);
    if(!files::detail::absolute_no_reparse(dir,true))return E_ACCESSDENIED;
    const auto removePath=dir/(L"remove-"+std::wstring(removed.begin(),removed.end())+L".journal");
    const DWORD attrs=GetFileAttributesW(removePath.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES){const DWORD error=GetLastError();return (error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)?S_OK:HRESULT_FROM_WIN32(error);}
    package_categories::WindowsStore categories;durable_journal::Store forward(removePath,id);durable_journal::Document doc;
    HRESULT hr=forward.read(id,&doc);if(hr!=S_OK)return hr;
    durable_journal::Store reverse(dir/(L"restore-"+std::wstring(restore.begin(),restore.end())+L".journal"),id);
    msi_modern_categories::DurableObserver observer(reverse,id);
    if(!doc.committed)return msi_modern_categories::recover_partial(categories,forward,id,false,observer);
    hr=msi_modern_categories::inspect_committed(categories,forward,id,false);if(hr!=S_OK)return hr;
    package_categories::Categories current;hr=package_categories::capture(categories,&current);if(hr!=S_OK)return hr;
    return msi_modern_categories::transition(categories,current,true,observer);
}

static bool metadata_string(HKEY key,const wchar_t* name,std::wstring& out) {
    wchar_t value[512]{};DWORD bytes=sizeof(value),type=0;
    if(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(value),&bytes)!=ERROR_SUCCESS ||
       type!=REG_SZ || bytes<sizeof(wchar_t) || bytes>sizeof(value) || bytes%sizeof(wchar_t)!=0 ||
       value[bytes/sizeof(wchar_t)-1]!=L'\0')return false;
    const size_t length=bytes/sizeof(wchar_t)-1;
    if(wcslen(value)!=length)return false;
    out.assign(value,length);return true;
}

extern "C" __declspec(dllexport) UINT __stdcall PrepareRemoval(MSIHANDLE install) {
    try {
        std::wstring root,sid,token,owner,recordedSid,recordedToken,recordedVersion,recordedHash;
        if(!property(install,L"INSTALLROOT",root)||!property(install,L"LIUKAIINSTALLTOKEN",token)||
           !elevated_process_user(sid)||root.find(L'|')!=std::wstring::npos)return failed(install,L"remove-context",E_ACCESSDENIED);
        HKEY key=nullptr;LONG open=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\LiuKai\\MSI\\{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}",0,KEY_READ|KEY_WOW64_64KEY,&key);
        if(open!=ERROR_SUCCESS)return failed(install,L"remove-metadata",HRESULT_FROM_WIN32(open));
        const bool ok=metadata_string(key,L"Owner",owner)&&metadata_string(key,L"Sid",recordedSid)&&
            metadata_string(key,L"Token",recordedToken)&&metadata_string(key,L"Version",recordedVersion)&&metadata_string(key,L"ManifestSha256",recordedHash);
        RegCloseKey(key);
        const std::wstring expectedVersion(kWixRuntimeVersion,kWixRuntimeVersion+strlen(kWixRuntimeVersion));
        const std::wstring expectedHash(kWixManifestSha256,kWixManifestSha256+strlen(kWixManifestSha256));
        if(!ok||owner!=L"LiuKai-TSF-v1"||recordedSid!=sid||recordedToken!=token||
           recordedVersion!=expectedVersion||recordedHash!=expectedHash)return failed(install,L"remove-identity",HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH));
        std::string undo,restore;if(!durable_cli::detail::make_token(&undo)||!durable_cli::detail::make_token(&restore)||undo==restore)return failed(install,L"remove-token",E_FAIL);
        const UINT rebootMode=MsiSetMode(install,MSIRUNMODE_REBOOTATEND,TRUE);
        if(rebootMode!=ERROR_SUCCESS)return failed(install,L"remove-reboot-mode",HRESULT_FROM_WIN32(rebootMode));
        const std::wstring data=sid+L"|"+root+L"|"+recordedVersion+L"|"+token+L"|"+std::wstring(undo.begin(),undo.end());
        const std::wstring restoreData=data+L"|"+std::wstring(restore.begin(),restore.end());
        return prepare_failure_collection(install,L"CollectRemovalFailure",sid,root) &&
            MsiSetPropertyW(install,L"RemoveRegistration",data.c_str())==ERROR_SUCCESS &&
            MsiSetPropertyW(install,L"RetireFiles",data.c_str())==ERROR_SUCCESS &&
            MsiSetPropertyW(install,L"UndoRetireFiles",data.c_str())==ERROR_SUCCESS &&
            MsiSetPropertyW(install,L"CommitRetireFiles",data.c_str())==ERROR_SUCCESS &&
            MsiSetPropertyW(install,L"UndoRemoval",restoreData.c_str())==ERROR_SUCCESS?ERROR_SUCCESS:ERROR_INSTALL_FAILURE;
    }catch(...){return failed(install,L"remove-prepare-exception",E_FAIL);}
}

extern "C" __declspec(dllexport) UINT __stdcall PrepareFreshRegistration(MSIHANDLE install) {
    std::wstring root,version,sid;std::string token,undo;
    HKEY existing=nullptr;
    const LONG probe=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\LiuKai\\MSI\\{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}",0,KEY_READ|KEY_WOW64_64KEY,&existing);
    if(existing)RegCloseKey(existing);
    if(probe!=ERROR_FILE_NOT_FOUND&&probe!=ERROR_PATH_NOT_FOUND)return failed(install,L"existing-msi-metadata",probe==ERROR_SUCCESS?HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS):HRESULT_FROM_WIN32(probe));
    if(!property(install,L"INSTALLROOT",root)||!property(install,L"LIUKAIRUNTIMEVERSION",version)||
       !elevated_process_user(sid)||root.find(L'|')!=std::wstring::npos||version.find(L'|')!=std::wstring::npos||
       !durable_cli::detail::make_token(&token)||!durable_cli::detail::make_token(&undo)||token==undo)return failed(install,L"prepare",E_INVALIDARG);
    PWSTR programFiles=nullptr;HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programFiles);
    if(hr!=S_OK)return failed(install,L"prepare-program-files",hr);
    const auto canonical=(std::filesystem::path(programFiles)/L"LiuKai").lexically_normal();CoTaskMemFree(programFiles);
    if(!detail::same_path(canonical.wstring(),msi_registration::normalized_root(root).wstring()))return failed(install,L"prepare-root",E_INVALIDARG);
    hr=msi_registration::require_absent_root(canonical);if(hr!=S_OK)return failed(install,L"prepare-existing-root",hr);
    root=canonical.wstring();
    const std::wstring data=sid+L"|"+root+L"|"+version+L"|"+std::wstring(token.begin(),token.end())+L"|"+std::wstring(undo.begin(),undo.end());
    return prepare_failure_collection(install,L"CollectInstallFailure",sid,root) &&
        MsiSetPropertyW(install,L"RecoverLegacyOrphan",data.c_str())==ERROR_SUCCESS &&
        MsiSetPropertyW(install,L"RegisterFresh",data.c_str())==ERROR_SUCCESS &&
        MsiSetPropertyW(install,L"RollbackFresh",data.c_str())==ERROR_SUCCESS &&
        MsiSetPropertyW(install,L"LIUKAIINSTALLTOKEN",std::wstring(token.begin(),token.end()).c_str())==ERROR_SUCCESS &&
        MsiSetPropertyW(install,L"LIUKAIINSTALLSID",sid.c_str())==ERROR_SUCCESS ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
}

extern "C" __declspec(dllexport) UINT __stdcall RecoverLegacyOrphan(MSIHANDLE install) {
    try {
        std::wstring data,sid;if(!property(install,L"CustomActionData",data)||!elevated_process_user(sid))return failed(install,L"legacy-recovery-context",E_ACCESSDENIED);
        std::vector<std::wstring> parts;size_t start=0;
        for(size_t i=0;i<=data.size();++i)if(i==data.size()||data[i]==L'|'){parts.push_back(data.substr(start,i-start));start=i+1;}
        if(parts.size()!=5||parts[0]!=sid)return failed(install,L"legacy-recovery-identity",E_ACCESSDENIED);
        const std::string version(parts[2].begin(),parts[2].end());
        if(version!=kWixRuntimeVersion)return failed(install,L"legacy-recovery-version",E_INVALIDARG);
        PWSTR programs=nullptr;HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programs);
        if(hr!=S_OK)return failed(install,L"legacy-recovery-program-files",hr);
        const auto root=(std::filesystem::path(programs)/L"LiuKai").lexically_normal();CoTaskMemFree(programs);
        if(!detail::same_path(root.wstring(),msi_registration::normalized_root(parts[1]).wstring()))return failed(install,L"legacy-recovery-root",E_INVALIDARG);
        bool orphan=false;hr=liukai::wix_legacy_recovery::orphan_present(orphan);
        if(hr!=S_OK)return failed(install,L"legacy-recovery-probe",hr);
        if(!orphan){trace_stage(install,L"legacy-recovery-not-needed");return ERROR_SUCCESS;}
        liukai::wix_legacy_recovery::LockedFiles locks;
        if(!liukai::wix_legacy_recovery::verify_helpers(root,version,kWixManifestSha256,kWixHelperSha256,kWixRecoveryProof,locks))return failed(install,L"legacy-recovery-integrity",HRESULT_FROM_WIN32(ERROR_CRC));
        trace_stage(install,L"legacy-recovery-verified");
        PWSTR local=nullptr;if(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local)!=S_OK)return failed(install,L"legacy-recovery-log-location",E_FAIL);
        const auto logDir=std::filesystem::path(local)/L"LiuKai"/L"installer-logs";CoTaskMemFree(local);
        std::wstring report;
        const DWORD exit=liukai::wix_legacy_recovery::run_script(root,sid,parts[2],kWixManifestSha256,kWixHelperSha256,kWixHistorySha256,kWixRecoveryProof,logDir,&report);
        if(!report.empty())trace_stage(install,(std::wstring(L"legacy-recovery-log=")+report).c_str());
        if(exit!=ERROR_SUCCESS)return failed(install,L"legacy-recovery-script",HRESULT_FROM_WIN32(exit));
        trace_stage(install,L"legacy-recovery-complete");return ERROR_SUCCESS;
    }catch(...){return failed(install,L"legacy-recovery-exception",E_FAIL);}
}
extern "C" __declspec(dllexport) UINT __stdcall RegisterFresh(MSIHANDLE install) {
    try {
        std::wstring data,sid;if(!property(install,L"CustomActionData",data)||!elevated_process_user(sid))return failed(install,L"context",E_ACCESSDENIED);
        std::vector<std::wstring> parts;size_t start=0;
        for(size_t i=0;i<=data.size();++i)if(i==data.size()||data[i]==L'|'){parts.push_back(data.substr(start,i-start));start=i+1;}
        if(parts.size()!=5||parts[0]!=sid)return failed(install,L"identity",E_ACCESSDENIED);
        std::string version(parts[2].begin(),parts[2].end()),token(parts[3].begin(),parts[3].end());
        if(version!=kWixRuntimeVersion||!files::detail::safe_label(version)||token.size()!=32||
           !std::all_of(token.begin(),token.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))return failed(install,L"arguments",E_INVALIDARG);
        PWSTR programFiles=nullptr;
        HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programFiles);
        if(hr!=S_OK)return failed(install,L"program-files",hr);
        const auto canonical=(std::filesystem::path(programFiles)/L"LiuKai").lexically_normal();CoTaskMemFree(programFiles);
        if(!detail::same_path(canonical.wstring(),msi_registration::normalized_root(parts[1]).wstring()))return failed(install,L"install-root",E_INVALIDARG);
        const auto manifest=canonical/L"versions"/std::filesystem::path(version)/L"package-manifest.json";
        if(!files::detail::absolute_no_reparse(manifest,true))return failed(install,L"manifest-path",E_ACCESSDENIED);
        const std::string hash=files::detail::sha256_file(manifest);
        if(hash!=kWixManifestSha256)return failed(install,L"manifest-hash",HRESULT_FROM_WIN32(ERROR_CRC));
        dev_host_migration_history::Lock lock;hr=lock.acquire();if(hr!=S_OK)return failed(install,L"lock",hr);
        if((hr=dev_host_migration_history::inspect(canonical))!=S_OK)return failed(install,L"legacy-history",hr);
        std::wstring helper;if(!verified_helper(canonical,&helper))return failed(install,L"helper-integrity",HRESULT_FROM_WIN32(ERROR_CRC));
        independent_categories::ScopedHelperExecutable helperScope(helper);
        trace_stage(install,L"before-com-initialize");
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(apartment))return failed(install,L"com",apartment);
        StageOnDestruction afterApartment{install,L"com-uninitialized"};
        struct Apartment{~Apartment(){CoUninitialize();}} apartmentGuard;
        StageOnDestruction afterTsf{install,L"tsf-objects-released"};
        WindowsTsfSharedApi api;if((hr=api.ready())!=S_OK)return failed(install,L"tsf",hr);
        RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,L"LiuKai-TSF-v1");
        WindowsTsfSharedStore shared(api,markers,L"LiuKai-TSF-v1");
        std::unique_ptr<WindowsInstallerBackend> backend;
        hr=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,L"LiuKai-TSF-v1",shared,&backend,true,canonical.wstring());
        if(hr!=S_OK)return failed(install,L"backend",hr);
        const auto projection=[&backend](Resource r,const ResourceState& state,ResourceState* out){return backend->project_apply_state(r,state,out);};
        if((hr=durable_cli::detail::inspect_machine_history(canonical,"LiuKai-TSF-v1",projection))!=S_OK)return failed(install,L"machine-history",hr);
        profile_uninstall::WindowsStore raw(boshiamy::profile_display::windows_detail::profile_key_path());
        profile_uninstall::UserState user;bool machine=false;
        if((hr=raw.machine_present(&machine))!=S_OK||(hr=raw.read_user(&user))!=S_OK)return failed(install,L"profile-state",hr);
        if(machine||user.present)return failed(install,L"legacy-profile",HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        durable_journal::Identity identity;identity.schema_version=3;identity.owner="LiuKai-TSF-v1";identity.token=token;
        identity.package_version=version;identity.package_manifest_sha256=hash;identity.install_root=canonical.wstring();
        const auto base=canonical.parent_path()/durable_journal::detail::ledger_namespace(identity)/L"LiuKai-TSF-v1";
        if(!durable_cli::detail::ensure_directory_chain(base))return failed(install,L"ledger-parent",E_ACCESSDENIED);
        const auto ledger=base/std::filesystem::path(token);identity.storage_root=ledger.wstring();
        if(!CreateDirectoryW(ledger.c_str(),nullptr))return failed(install,L"ledger-create",HRESULT_FROM_WIN32(GetLastError()));
        InstallRequest request;request.owner=L"LiuKai-TSF-v1";request.include_x86=false;request.include_machine_com_x64=true;
        request.x64_binary_path=(canonical/L"versions"/std::filesystem::path(version)/L"x64"/L"BoshiamyTsf.dll").wstring();
        request.icon_path=(canonical/L"shared"/L"branding.dll").wstring();
        const auto result=msi_registration::install_fresh_durable(*backend,request,identity);
        if(result.operation!=S_OK||result.cleanup!=S_OK)return failed(install,L"registration",result.operation!=S_OK?result.operation:result.cleanup);
        trace_stage(install,L"base-registration-committed");
        hr=enable_modern_categories(canonical,token,version);
        if(hr!=S_OK)return failed(install,L"modern-categories",hr);
        trace_stage(install,L"modern-categories-committed");
        return ERROR_SUCCESS;
    }catch(...){return failed(install,L"exception",E_FAIL);}
}

static UINT remove_owned_registration(MSIHANDLE install,bool allow_missing) {
    try {
        std::wstring data,sid;if(!property(install,L"CustomActionData",data)||!elevated_process_user(sid))return failed(install,L"rollback-context",E_ACCESSDENIED);
        std::vector<std::wstring> parts;size_t start=0;
        for(size_t i=0;i<=data.size();++i)if(i==data.size()||data[i]==L'|'){parts.push_back(data.substr(start,i-start));start=i+1;}
        if(parts.size()!=5||parts[0]!=sid)return failed(install,L"rollback-identity",E_ACCESSDENIED);
        std::string version(parts[2].begin(),parts[2].end()),token(parts[3].begin(),parts[3].end()),undo(parts[4].begin(),parts[4].end());
        auto valid_token=[](const std::string& v){return v.size()==32&&std::all_of(v.begin(),v.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});};
        if(version!=kWixRuntimeVersion||!valid_token(token)||!valid_token(undo)||token==undo)return failed(install,L"rollback-arguments",E_INVALIDARG);
        PWSTR programFiles=nullptr;HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programFiles);
        if(hr!=S_OK)return failed(install,L"rollback-program-files",hr);
        const auto root=(std::filesystem::path(programFiles)/L"LiuKai").lexically_normal();CoTaskMemFree(programFiles);
        if(!detail::same_path(root.wstring(),msi_registration::normalized_root(parts[1]).wstring()))return failed(install,L"rollback-root",E_INVALIDARG);
        dev_host_migration_history::Lock lock;hr=lock.acquire();if(hr!=S_OK)return failed(install,L"rollback-lock",hr);
        durable_journal::Identity original;original.schema_version=3;original.owner="LiuKai-TSF-v1";original.token=token;
        original.package_version=version;original.package_manifest_sha256=kWixManifestSha256;original.install_root=root.wstring();
        const auto base=root.parent_path()/durable_journal::detail::ledger_namespace(original)/L"LiuKai-TSF-v1";
        const auto ledger=base/std::filesystem::path(token);original.storage_root=ledger.wstring();
        DWORD attrs=GetFileAttributesW(ledger.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES) {DWORD error=GetLastError();return allow_missing&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)?ERROR_SUCCESS:failed(install,L"rollback-ledger",HRESULT_FROM_WIN32(error));}
        if(!files::detail::absolute_no_reparse(ledger,true))return failed(install,L"rollback-ledger-path",E_ACCESSDENIED);
        const auto journal=ledger/L"transaction.journal", snapshot=ledger/L"transaction.snapshot";
        std::error_code error;const bool hasJournal=std::filesystem::exists(journal,error);if(error)return failed(install,L"rollback-journal",E_FAIL);
        const bool hasSnapshot=std::filesystem::exists(snapshot,error);if(error)return failed(install,L"rollback-snapshot",E_FAIL);
        if(!hasJournal&&!hasSnapshot&&allow_missing)return ERROR_SUCCESS; // No observer began; no mutation can have occurred.
        if(!hasJournal||!hasSnapshot)return failed(install,L"rollback-incomplete",HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        std::wstring helper;if(!verified_helper(root,&helper))return failed(install,L"rollback-helper-integrity",HRESULT_FROM_WIN32(ERROR_CRC));
        independent_categories::ScopedHelperExecutable helperScope(helper);
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(apartment))return failed(install,L"rollback-com",apartment);
        struct Apartment{~Apartment(){CoUninitialize();}} apartmentGuard;
        WindowsTsfSharedApi api;if((hr=api.ready())!=S_OK)return failed(install,L"rollback-tsf",hr);
        RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,L"LiuKai-TSF-v1");
        WindowsTsfSharedStore shared(api,markers,L"LiuKai-TSF-v1");std::unique_ptr<WindowsInstallerBackend> backend;
        hr=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,L"LiuKai-TSF-v1",shared,&backend,true,root.wstring());
        if(hr!=S_OK)return failed(install,L"rollback-backend",hr);
        if((hr=msi_registration::inspect_committed_fresh(*backend,original))!=S_OK)return failed(install,L"rollback-verification",hr);
        if((hr=disable_modern_categories(root,token,version,undo,allow_missing))!=S_OK)return failed(install,L"rollback-modern-categories",hr);
        auto compensation=original;compensation.token=undo;const auto undoLedger=base/std::filesystem::path(undo);compensation.storage_root=undoLedger.wstring();
        if(!CreateDirectoryW(undoLedger.c_str(),nullptr))return failed(install,L"rollback-create",HRESULT_FROM_WIN32(GetLastError()));
        InstallRequest request;request.owner=L"LiuKai-TSF-v1";request.include_x86=false;request.include_machine_com_x64=true;
        request.x64_binary_path=(root/L"versions"/std::filesystem::path(version)/L"x64"/L"BoshiamyTsf.dll").wstring();
        request.expected_x64_path=request.x64_binary_path;request.icon_path=(root/L"shared"/L"branding.dll").wstring();
        const auto result=msi_registration::rollback_committed_fresh(*backend,request,original,compensation);
        if(result.operation!=S_OK||result.cleanup!=S_OK)return failed(install,L"rollback-registration",result.operation!=S_OK?result.operation:result.cleanup);
        return ERROR_SUCCESS;
    }catch(...){return failed(install,L"rollback-exception",E_FAIL);}
}
extern "C" __declspec(dllexport) UINT __stdcall RollbackFresh(MSIHANDLE install) {return remove_owned_registration(install,true);}
extern "C" __declspec(dllexport) UINT __stdcall RemoveRegistration(MSIHANDLE install) {return remove_owned_registration(install,false);}

struct RetireContext {
    std::filesystem::path tsf, branding, retiredTsf, retiredBranding;
};
static bool retire_context(MSIHANDLE install, RetireContext& out) {
    std::wstring data,sid;
    if(!property(install,L"CustomActionData",data)||!elevated_process_user(sid))return false;
    std::vector<std::wstring> parts;size_t start=0;
    for(size_t i=0;i<=data.size();++i)if(i==data.size()||data[i]==L'|'){parts.push_back(data.substr(start,i-start));start=i+1;}
    if(parts.size()!=5||parts[0]!=sid)return false;
    const std::wstring version(kWixRuntimeVersion,kWixRuntimeVersion+strlen(kWixRuntimeVersion));
    auto valid=[](const std::wstring& s){return s.size()==32&&std::all_of(s.begin(),s.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');});};
    if(parts[2]!=version||!valid(parts[3])||!valid(parts[4])||parts[3]==parts[4])return false;
    PWSTR programFiles=nullptr;if(SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programFiles)!=S_OK)return false;
    const auto parent=std::filesystem::path(programFiles).lexically_normal();CoTaskMemFree(programFiles);
    const auto root=(parent/L"LiuKai").lexically_normal();
    if(!detail::same_path(root.wstring(),msi_registration::normalized_root(parts[1]).wstring()))return false;
    out.tsf=root/L"versions"/version/L"x64"/L"BoshiamyTsf.dll";
    out.branding=root/L"shared"/L"branding.dll";
    const auto stem=L".liukai-retired-"+parts[4];
    out.retiredTsf=parent/(stem+L"-tsf.dll");out.retiredBranding=parent/(stem+L"-branding.dll");
    return true;
}
extern "C" __declspec(dllexport) UINT __stdcall RetireFiles(MSIHANDLE install) {
    try {
        RetireContext c;if(!retire_context(install,c))return failed(install,L"retire-context",E_ACCESSDENIED);
        HRESULT hr=liukai::retired_files::retire(c.tsf,c.retiredTsf,kWixTsfSha256);
        if(hr!=S_OK)return failed(install,L"retire-tsf",hr);
        hr=liukai::retired_files::retire(c.branding,c.retiredBranding,kWixBrandingSha256);
        if(hr!=S_OK)return failed(install,L"retire-branding",hr);
        return ERROR_SUCCESS;
    }catch(...){return failed(install,L"retire-exception",E_FAIL);}
}
extern "C" __declspec(dllexport) UINT __stdcall UndoRetireFiles(MSIHANDLE install) {
    try {
        RetireContext c;if(!retire_context(install,c))return failed(install,L"retire-undo-context",E_ACCESSDENIED);
        const HRESULT a=liukai::retired_files::restore(c.branding,c.retiredBranding,kWixBrandingSha256);
        const HRESULT b=liukai::retired_files::restore(c.tsf,c.retiredTsf,kWixTsfSha256);
        return a==S_OK&&b==S_OK?ERROR_SUCCESS:failed(install,L"retire-undo",a!=S_OK?a:b);
    }catch(...){return failed(install,L"retire-undo-exception",E_FAIL);}
}
extern "C" __declspec(dllexport) UINT __stdcall CommitRetireFiles(MSIHANDLE install) {
    try {
        RetireContext c;if(!retire_context(install,c))return failed(install,L"retire-commit-context",E_ACCESSDENIED);
        const HRESULT a=liukai::retired_files::schedule_retired(c.retiredTsf,kWixTsfSha256);
        const HRESULT b=liukai::retired_files::schedule_retired(c.retiredBranding,kWixBrandingSha256);
        if(a!=S_OK||b!=S_OK)return failed(install,L"retire-commit-cleanup",a!=S_OK?a:b);
        return ERROR_SUCCESS;
    }catch(...){return failed(install,L"retire-commit-exception",E_FAIL);}
}

extern "C" __declspec(dllexport) UINT __stdcall UndoRemoval(MSIHANDLE install) {
    try {
        std::wstring data,sid;if(!property(install,L"CustomActionData",data)||!elevated_process_user(sid))return failed(install,L"undo-context",E_ACCESSDENIED);
        std::vector<std::wstring> parts;size_t start=0;
        for(size_t i=0;i<=data.size();++i)if(i==data.size()||data[i]==L'|'){parts.push_back(data.substr(start,i-start));start=i+1;}
        if(parts.size()!=6||parts[0]!=sid)return failed(install,L"undo-identity",E_ACCESSDENIED);
        std::string version(parts[2].begin(),parts[2].end()),installedToken(parts[3].begin(),parts[3].end()),removedToken(parts[4].begin(),parts[4].end()),restoreToken(parts[5].begin(),parts[5].end());
        auto valid=[](const std::string& v){return v.size()==32&&std::all_of(v.begin(),v.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});};
        if(version!=kWixRuntimeVersion||!valid(installedToken)||!valid(removedToken)||!valid(restoreToken)||
           installedToken==removedToken||installedToken==restoreToken||removedToken==restoreToken)return failed(install,L"undo-arguments",E_INVALIDARG);
        PWSTR programFiles=nullptr;HRESULT hr=SHGetKnownFolderPath(FOLDERID_ProgramFiles,KF_FLAG_DEFAULT,nullptr,&programFiles);
        if(hr!=S_OK)return failed(install,L"undo-program-files",hr);
        const auto root=(std::filesystem::path(programFiles)/L"LiuKai").lexically_normal();CoTaskMemFree(programFiles);
        if(!detail::same_path(root.wstring(),msi_registration::normalized_root(parts[1]).wstring()))return failed(install,L"undo-root",E_INVALIDARG);
        dev_host_migration_history::Lock lock;hr=lock.acquire();if(hr!=S_OK)return failed(install,L"undo-lock",hr);
        durable_journal::Identity installed;installed.schema_version=3;installed.owner="LiuKai-TSF-v1";installed.token=installedToken;
        installed.package_version=version;installed.package_manifest_sha256=kWixManifestSha256;installed.install_root=root.wstring();
        const auto base=root.parent_path()/durable_journal::detail::ledger_namespace(installed)/L"LiuKai-TSF-v1";
        installed.storage_root=(base/std::filesystem::path(installedToken)).wstring();
        auto removed=installed;removed.token=removedToken;const auto removedLedger=base/std::filesystem::path(removedToken);removed.storage_root=removedLedger.wstring();
        const DWORD attrs=GetFileAttributesW(removedLedger.c_str());
        bool removalLedgerExists=attrs!=INVALID_FILE_ATTRIBUTES;
        if(!removalLedgerExists){DWORD error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)return failed(install,L"undo-ledger",HRESULT_FROM_WIN32(error));}
        if(removalLedgerExists&&!files::detail::absolute_no_reparse(removedLedger,true))return failed(install,L"undo-ledger-path",E_ACCESSDENIED);
        std::wstring helper;if(!verified_helper(root,&helper))return failed(install,L"undo-helper-integrity",HRESULT_FROM_WIN32(ERROR_CRC));
        independent_categories::ScopedHelperExecutable helperScope(helper);
        const HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(apartment))return failed(install,L"undo-com",apartment);
        struct Apartment{~Apartment(){CoUninitialize();}} apartmentGuard;
        WindowsTsfSharedApi api;if((hr=api.ready())!=S_OK)return failed(install,L"undo-tsf",hr);
        RegistryOwnerMarkerStore markers(HKEY_CURRENT_USER,L"Software\\LiuKai\\Installer",kInstallerTsfClsid,L"LiuKai-TSF-v1");
        WindowsTsfSharedStore shared(api,markers,L"LiuKai-TSF-v1");std::unique_ptr<WindowsInstallerBackend> backend;
        hr=WindowsInstallerBackend::open_scope(kInstallerTsfClsid,L"LiuKai-TSF-v1",shared,&backend,true,root.wstring());
        if(hr!=S_OK)return failed(install,L"undo-backend",hr);
        durable_journal::Store journal(removedLedger/L"transaction.journal",removed,removedLedger);durable_journal::Document doc;
        hr=removalLedgerExists?journal.read(removed,&doc):HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if(hr!=S_OK||!doc.committed) {
            if(msi_registration::inspect_committed_fresh(*backend,installed)!=S_OK)return failed(install,L"undo-incomplete-removal",HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
            hr=restore_modern_categories(root,installedToken,version,removedToken,restoreToken);
            return hr==S_OK?ERROR_SUCCESS:failed(install,L"undo-modern-categories",hr);
        }
        ResourceStates checked{};hr=msi_registration::inspect_committed_removal(*backend,installed,removed,&checked);
        if(hr!=S_OK)return failed(install,L"undo-verification",hr);
        auto restoration=installed;restoration.token=restoreToken;const auto restoreLedger=base/std::filesystem::path(restoreToken);restoration.storage_root=restoreLedger.wstring();
        if(!CreateDirectoryW(restoreLedger.c_str(),nullptr))return failed(install,L"undo-create",HRESULT_FROM_WIN32(GetLastError()));
        const auto result=msi_registration::restore_committed_removal(*backend,installed,removed,restoration);
        if(result.operation!=S_OK||result.cleanup!=S_OK)return failed(install,L"undo-restore",result.operation!=S_OK?result.operation:result.cleanup);
        hr=restore_modern_categories(root,installedToken,version,removedToken,restoreToken);
        if(hr!=S_OK)return failed(install,L"undo-modern-categories",hr);
        return ERROR_SUCCESS;
    }catch(...){return failed(install,L"undo-exception",E_FAIL);}
}
