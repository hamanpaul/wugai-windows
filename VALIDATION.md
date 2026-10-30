# R60 驗收範圍

日期：2026-10-26  
版本：`0.0.0-dev.20261026.r60.machine-com-x64`  
發行定位：Windows x64 prerelease，tag `r60`。

使用者在已安裝的 R60 上確認「三項都正常」：

- Windows Search：`a + Space`、`oek + Space` 中文輸入正常。
- Microsoft Store：`a + Space`、`oek + Space` 中文輸入正常。
- 工作列獨立中／英模式圖示正常。

安裝後 12 個套件檔案的 SHA-256 核對通過。全新獨立程序確認 HKCU64／HKLM64 均指向 R60 DLL，機器層級的 owner、安裝帳號 SID、安裝根目錄及 64 位元視圖符合預期；registered、immersive、input_mode、system_tray 與 user_enabled 讀回通過。這些讀回與上述使用者實測分開記錄。

Terminal 沿用先前正常結果，本輪未重新測試。不宣稱完整 77 項 backlog、官版 1:1 全功能或所有宿主情境均已驗收。公開來源匯出副本未另跑完整 clean-checkout build／test。

已驗安裝檔：`WuGai-R60-x64-Setup.exe`  
SHA-256：`b65fa61ff1fc26580c603dd82093c0081d3d097c06a0ccdad8d0c4803fd3bda4`
# R60.1 安裝器修正驗證（2026-10-26）

R60 在新註冊的 TSF Profile 沒有 `HiddenInSettingUI`、`SubItemInSettingUI` 時，讀圖示路徑會連帶要求完整 metadata 而失敗。修正版只讀取 HKLM64 的 `IconFile`；完整顯示快照、所有權檢查及既有交易格式維持原有規則。

- 同一個受影響 Profile 的唯讀對照：R60 原始 `WindowsTsfSharedApi::profile_icon_path` 回傳 `2147942402`（`0x80070002`），修正版回傳 `0` 並取得原圖示路徑。沒有補寫缺省欄位。
- `profile_icon_readback_tests.cpp`：缺少 settings metadata、無關欄位型別錯誤仍可讀取圖示；缺少、空白、型別錯誤及內嵌 NUL 的 `IconFile` 均拒絕。測試只建立及清除獨立 HKCU 測試鍵。
- 既有 command-line、dual-architecture lifecycle、production status report、durable singleton recovery 測試通過。
- 安裝腳本讀回測試 18 項、套件安裝入口測試 21 項通過。
- R60.1 release bundle 的 16 個檔案與 manifest 一致，且包含新編譯的 CLI；安裝計畫檢查通過。

本輪尚未宣告乾淨 Windows / 新 SID 的端到端安裝驗收，也沒有重新驗收 Search、Store 或中英圖示。R60.1 只重建安裝 CLI 與 Setup 外殼，其餘已校驗 payload 沿用 R60。

使用 `native/build-release.ps1 -SourceBundle <解壓後的 R60 bundle> -InstallerCLI <修正版 CLI> -Version <新版本> -OutputDir <不存在的輸出目錄>` 可重新封裝。輸出包含安裝 EXE、release bundle 及 SHA256SUMS.txt；不會發布或安裝。

# R60.1 跨電腦失敗追查（2026-10-26）

使用者確認另一台執行 R60.1，提供的 `20261026T133315Z-a966877d91f041c1a7941ecb26552d44.log`
記錄 operation=`0x8007000D`（ERROR_INVALID_DATA）、cleanup=`0x8007051A`（ERROR_REVISION_MISMATCH）。
最終所有資源 absent 且 readback=0；摘要沒有第一個失敗階段，不能據此宣稱端到端安裝已修復。

已重現獨立的回復證據驗證缺陷：shared resource 的 apply observation 使用投影後的狀態，
restore 使用完整原始狀態，原本卻直接比較兩者雜湊。修正將 apply observation 與同格式的
prestate 比較；跨格式的 restore 仍要求有效的 before hash、精確的原始 snapshot 目標及讀回。
執行時 rollback 的狀態比對保護沒有放寬。此缺陷可造成 cleanup 錯誤，尚未證明是他機的完整原因。

另外補強 Setup 持久 LOG（版本、SHA-256、OS build、階段、exception、exit code）、
套件部署預設 diagnostic trace，以及 profile 註冊／啟用／圖示／最終驗證的分段 trace。
trace 不增加 TSF API 呼叫，也不更改啟用語意。新增唯讀紀錄收集腳本，操作方式見
`native/INSTALL-DIAGNOSTICS.md`；本機實測可讀取原始 journal / snapshot 並輸出 ZIP。

驗證包含 production projection 的正負案例、profile trace 與原 API 呼叫一致性、既有 status report、
Setup log helper 及 package wrapper 測試。整合 CLI 已編譯。這些驗證沒有執行真機安裝，
也沒有處理原本未完成的交易。沒有重新封裝或發布新版本。

仍需另一台的 `transaction.journal` / `transaction.snapshot`，才能判定最初的 ERROR_INVALID_DATA。
R60.1 安裝檔目前無 Authenticode 簽章；Google Drive 警告未取得完整文字，不能認定為誤判，
也不能將未簽章直接視為 Google 的偵測原因。

# R60.2 卸載入口與診斷（2026-10-28）

新增 Setup `--uninstall`、安裝後的 `Uninstall.exe` 及 Windows 應用程式解除安裝入口。
只有部署與獨立註冊讀回成功才發布入口；維護檔置於安裝目錄旁的 `.maintenance`，
不改變原套件 manifest 或受管理根目錄的固定檔案規則。安裝與卸載使用同一外層 mutex，
卸載器先交接到暫存副本，避免自己的執行檔阻擋刪除。

卸載先嚴格驗證整棵安裝目錄、所有版本、帳號、所有權及交易歷史，保存可核對的完整備份，
再移除 modern-host 類別及既有七個 lifecycle resources。所有類別、原始 Profile 登錄葉節點、
API 與 COM 都必須在獨立程序讀回為 absent，才允許刪除程式檔案。modern-host 類別先行清理
不屬於七個資源的原子回復；若後續失敗，會如實保留卸載進度、套件與診斷資料。

- Native package fixture 通過：完整 inventory、多版本、未知檔案／類別、外來及借用註冊、
  SID／路徑不符、未完成 journal 拒絕；部分類別失敗、七資源失敗回復後重試，以及
  成功卸載、刪除 fixture 根目錄後保留原 ledger 再安裝。
- Profile 殘留清理、parser、status report、Windows backend 所有權測試通過；登錄測試
  限制於隔離的測試鍵，未安裝或卸載本機真實輸入法。
- PowerShell 卸載 proof、完整 inventory 及檔案復原測試 67 項通過；安裝讀回測試 20 項通過。
  一般刪檔例外會從已驗證備份恢復可再次卸載的檔案。程式在檔案清理階段遭強制終止或斷電時，
  尚不支援自動跨程序還原，應保留備份與 LOG 進行人工復原。
- 維護入口測試 114 項、維護入口中斷／回復測試 53 項、Setup 參數及交接測試 51 項通過。
  這些使用隔離的檔案 fixture 和模擬 Apps 登錄，沒有啟動真正的安裝或卸載程序。
- 此輪新 CLI 對使用者已清理電腦的唯讀檢查：七資源 absent、all_categories_count=0、
  profile_registry_absence_result=0、operation/cleanup/readback=0。

本機候選安裝檔 `WuGai-R60.2-x64-Setup.exe` 的 SHA-256 為
`23f3f0a715d2d550826e3d14c078e3aaa37603105acb6071d0c625d882ded2f4`。
已核對內嵌 ZIP 與 release manifest 的 18 個檔案；Windows 讀取簽章結果為 `NotSigned`。
候選檔尚未發布至 GitHub。

使用者設定、備份及歷史交易保留。舊失敗交易沒有被刪除、改寫或假造成功 receipt；
未解決的歷史交易仍可能阻止該目錄的後續安裝，需依原 journal / snapshot 處理。
此候選版不是另一台 `0x8007000D` 已修復的證明，也尚未通過乾淨 Windows／新 SID 的
真實安裝、卸載及重裝驗收。尚無 Authenticode 簽章，不能宣稱 Google Drive 警示已消除。

# 本機真實安裝追查（2026-10-28）

在確認本機程式目錄與所有註冊已不存在、核對原 journal / snapshot 與備份的雜湊後，
將原失敗交易原樣封存到同一 ledger namespace 的 `manual-reconciliation`，另寫明確的
人工處置紀錄。此處置不是 compensation 成功證明；原 journal 沒有改寫。

- R60.2 實機部署在建立新交易之前失敗。管理員探針顯示 HKCU 的 Classes 可寫，
  HKLM 的 `Software\Classes` 以寫入權限開啟回傳 5，`Software\Classes\CLSID` 可寫。
  已把機器 COM 開啟範圍縮小到 CLSID 父鍵，隔離登錄的建立、讀取和移除測試通過，
  並保留相鄰 CLSID。
- R60.3 / R60.4 實機記錄在鍵盤類別 `RegisterCategory` 回傳 0 之後，同程序的
  類別列舉仍顯示 absent，導致 `0x8007000D`。新的獨立程序後來讀到該關係存在；
  已依特定失敗交易證據只移除該單一殘留。這是此電腦的直接證據，不足以推論另一台的主因。
- R60.5 類別獨立讀回後越過上述錯誤；後續 Profile capability 在不同時間點顯示
  `S_FALSE` 或尚缺必需位元。R60.9 為相關查詢加入有限重試，仍要求實際讀回能力位元；
  子程序讀回及能力稍後出現／超時的隔離測試通過，尚未完成新的實機驗收。
- R60.8 安裝失敗後，註冊已讀回 absent，但檔案 rollback 回傳 `0x80070005`，
  留下一筆未完成交易與部分檔案。`explorer.exe`、`ChatGPT.exe` 曾載入該版本的 DLL；
  這些檔案與 journal 保留，未強行關閉程式。受限復原腳本須確認載入者已釋放 DLL、
  精確檔案清單與雜湊、所有註冊及使用者帳號，才備份檔案並封存原交易。
- 本機候選 `WuGai-R60.9-x64-Setup.exe` SHA-256：
  `50c980222f05f185c1bb979c0d13bd026eff67d7f7e29ce1384825354982dd50`。
  18 個內嵌檔案與 manifest 已核對；CLI 參數、獨立 status report、durable singleton
  復原與套件卸載／重裝 fixture 回歸通過。尚未完成真實安裝或卸載測試，也未發布。

Google 通知確認 R60.1 被依惡意軟體或類似惡意內容政策限制，但未提供偵測名稱或命中原因。
Drive API 拒絕下載該受限制原檔；它與本機 R60.1 檔名及大小相同，但無法據此證明雜湊相同。
Microsoft Defender 對本機 R60.1 安裝檔和解包內容，以及後續候選安裝檔的自訂掃描沒有發現威脅；
這不證明 Google 的判定錯誤，也不證明檔案絕對安全。安裝檔未簽署，申請 Google 人工複查
仍需要檔案擁有者執行。

# Google 政策對照（2026-10-28）

Google Drive 的[惡意軟體和類似惡意內容政策](https://support.google.com/docs/answer/148505?hl=zh-Hant#malware)
禁止惡意程式、損害裝置或網路的內容及未經同意安裝軟體。僅依現有程式碼與本機檔案的檢查，
未發現可確定符合這些條件的惡意行為；這不是完整安全審計，也不能證明 Google 的判定有誤。

比對 Google 另行公布的[網站下載及不受歡迎軟體指引](https://developers.google.com/search/docs/monitor-debug/security/malware)，
R60.1 存在應改善的透明度：一般安裝在 UAC 授權後直接開始，沒有安裝前畫面清楚說明輸入法、
程式檔案及系統註冊變更；R60.1 也沒有隨套件提供易找的 Windows 解除安裝入口。
新版原始碼已加入解除安裝入口，但完整實機安裝／卸載驗收仍未完成；安裝前說明與明確同意
尚待補上。上述指引和 Drive 的處置並非同一份裁決，不能據此推斷 Google 的具體命中原因。

R60.1 的 Setup 會將內嵌 payload 展開至 `%TEMP%\LiuKai-Setup-<job>`，目前查閱的程式碼
沒有在結束時移除該解壓目錄；新版 Setup 也保留此行為，且安裝後的解除安裝入口會另建立
`%TEMP%\LiuKai-Uninstall-<job>\Uninstall.exe` 暫存工作程式，目前沒有看到其完成後清理。
因此「反安裝入口已加入」不等於所有程式元件已從暫存位置移除；這是應修正並實測的具體缺口，
但仍不能認定是 Google 此次判定的原因。使用者設定、備份與診斷紀錄是否保留須另外清楚告知。

R60.1 的安裝外殼會自我提權、解壓內嵌 ZIP，並以隱藏視窗執行 PowerShell，使用該子程序的
`ExecutionPolicy Bypass`；這些是實際行為，但本身不能證明是惡意程式或 Google 的觸發原因。
程式碼所採用的 TSF in-process COM 輸入法架構可能由 Windows 載入至其他應用程式，屬
[Microsoft 文件描述的正常整合方式](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)。
來源靜態搜尋未發現列舉的常見網路上傳 API，R60.1 的原生二進位靜態匯入表也未列出常見網路 DLL；
這不能排除動態載入或其他執行行為。來源中的可選執行期診斷紀錄採數值欄位，不包含按鍵文字。

Google 的[不受歡迎軟體指引](https://developers.google.com/search/docs/monitor-debug/security/malware)
明確指出未簽章本身不是標記為不受歡迎軟體的理由，同時仍建議使用可驗證的程式碼簽章。
該陳述不能推論 Drive 的惡意軟體偵測細節。需讓 Google 檢視被限制的確切檔案並提供更多資訊，
才有機會確認實際偵測原因。


## 2026-10-28: local setup and uninstall follow-up

- The old R60.8 partial files were removed after confirming the DLL was no longer mapped. The original failed transaction was archived unchanged, with a verified backup.
- R60.11 installed and passed independent installed-state checks. Its actual installed uninstaller removed registrations but failed after a stale same-process profile read; payload and worker temporary cleanup both logged completion. A fresh retry completed removal and verified absence.
- R60.12 changed profile readback globally to a fresh process. The actual installation exposed a transient false-absence immediately after registration, leaving an orphan profile and failed transaction. The profile was subsequently removed; a fresh status shows all seven registrations absent and profile registry absence. The failed transaction remains active pending elevated archival: the Windows UAC request was cancelled. Do not retry installation until history is resolved.
- R60.13 narrows fresh profile readback to after a successful profile unregistration; its candidate installer has not completed a real installation/uninstallation cycle. SHA-256: 87d9bf063907062d27d7f580526ccaf13a603d8f47dc523bc9e15f1ca4548898. Do not treat it as validated or claim it resolves Google Drive's malware verdict.
- Setup confirmation, extraction cleanup, worker cleanup and Windows PowerShell module-path handling are implemented. The Setup unit test passed 56 checks, including execution of a temporary worker cleanup fixture. The actual confirmation UI has not been interactively verified.


## 2026-10-28: ambiguous TSF readback follow-up

The R60.12 real-machine log showed a successful profile registration followed by a transient absent read. A later independent read found the profile present. An absent read immediately after a successful write is therefore not sufficient proof of rollback. When a successfully created profile or category remains pending, the shared store now returns ERROR_RETRY for an absent observation instead of treating it as a restored prestate. Bounded post-write readback retries use observed values and report ERROR_RETRY when the requested state is not observed; retries do not guarantee future state and do not manufacture an observation.

Isolated tests cover delayed profile visibility, refusing to report restoration while creation remains pending, recovery once the profile becomes observable, delayed and timed-out presence reads, and an end-to-end simulated transaction with an unreadable possibly mutated profile. The latter preserves its installed fixture files and original journal and rejects subsequent machine-scope installation history inspection. Package-uninstall and durable singleton regression fixtures also passed. These tests do not prove cross-process automatic recovery for every interruption and do not exercise real TSF mutation.

On this computer, a fresh read-only status observed all seven product registrations absent and profile registry absence; the prior failed transaction still causes history inspection to return ERROR_INVALID_STATE. No new installation or uninstallation was performed in this follow-up. The current source has not been packaged or validated in Windows Search or Microsoft Store. The previous R60 Search/Store user verification is historical and does not establish current-version behavior.

Candidate UI follow-up: GetPageIndex now supports the documented count-only call and reports S_OK when the provided array exactly fits; an undersized array is rejected without a partial write. Empty candidate selection returns S_FALSE. Isolated COM-object contract tests and compilation of the x64 service DLL passed. This is a contract correction, not evidence that it caused the Search/Store issue, and the rebuilt DLL has not been installed or tested in either application. Reference: https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcandidatelistuielement-getpageindex and https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview.

Failed-registration uncertainty follow-up: a registration API failure is now retained as unknown, rather than adopting an unmarked profile/category or declaring rollback complete; pending profile icon ownership must match the expected path. Simulated profile/category failures after mutation and a foreign-icon collision passed. PowerShell 5 and PowerShell 7 maintenance-recovery fixtures each passed 53 checks; PowerShell 7 JSON integers are normalized only when reading publication objects, while live-registry QWORD values remain rejected.

R60.14 candidate (not installed): release manifest and all 18 payload files, embedded ZIP, current scripts, lifecycle CLI and freshly compiled TSF DLL were hash-verified. Setup SHA-256 bfddc9f9d9099dc1e8bd13ce4b25610ad3b0ed58a92314f2ab568ef99a4b6512. Microsoft Defender custom scan with remediation disabled returned no threats; this does not determine a Google Drive verdict. A renewed UAC request to archive the exact manually reconciled R60.12 transaction returned Windows' cancellation error, so the archive operation did not run and real install/uninstall and Search/Store tests remain pending.


## 2026-10-28: mapped-DLL uninstall preflight

An exclusive file open is insufficient to establish that a Windows image is not mapped: the R60.8 failure supplied direct evidence of that gap. Before changing registrations, the uninstall wrapper now queries processes that tasklist reports loading BoshiamyTsf.dll, compares their actual module paths with all installed package DLL paths, and stops if a match or inaccessible process is found. The process inventory is only a point-in-time check; another process could load the DLL afterwards, so the existing backup and failure restoration remain necessary. This guard reports process names/PIDs in the log and does not close programs.

Isolated PowerShell tests passed 77 checks, including different-path DLLs, inaccessible processes, invalid process IDs and missing module data. A separate bounded test process actually loaded the candidate DLL by path, and the production helper identified that exact process and module without registering an input method. Real package uninstallation was not performed. R60.15 includes the guard and previous fixes; its embedded ZIP and 18 release payloads match their manifests and compiled artifacts. SHA-256: 62a08500442271972bcd420ef46185c6b998804b42cbb76f13eb721c113a6d70. Microsoft Defender reported no threats for a custom scan with remediation disabled; Google Drive classification is unknown. This candidate has not passed real install/uninstall or Search/Store input testing.

A separate outstanding limitation remains: if the uninstaller process or computer is abruptly stopped during recursive file removal, the in-process catch cannot run. Backups are retained, but automated cross-process restoration of that partial removal is not yet implemented or validated.

Repeatable mapped-module integration is now in native/tests/mapped_module_integration_tests.ps1 and uses native/tests/mapped_module_fixture.cpp. Windows PowerShell 5 ran it successfully: a short-lived fixture loaded only the workspace DLL; the production read-only helper found its exact PID and rejected a different path, without running an installer or touching TSF registration. Simulated inaccessible or inconsistent process-module inspection also stops uninstall before registration removal.


## 2026-10-28: interrupted uninstall file recovery

The uninstall file validators and restore helper are now shared by installation and uninstallation. After registration absence, verified package and maintenance backups are bound to a flushed, atomically renamed receipt under Program Files before deletion. A subsequent uninstall or install detects the receipt under the release mutex, checks the same user/root, original plan and maintenance hashes, live registration absence, and refuses unknown or changed files/Apps values before restoring; it holds read-only backup handles during revalidation and restoration. Successful removal or restoration clears the exact unchanged receipt. If validation fails, evidence and backups are retained. A failed temporary receipt write cannot create a partial final receipt.

A dedicated isolated test killed a child process after deleting test product and maintenance files, then a separate process restored them from the receipt. Other cases cover removal of the entire test tree, absent original maintenance, modified plan/backup/Apps entry, different SID, live registration, active backup writer, foreign file contents and duplicate receipt. Windows PowerShell 5 and PowerShell 7 each passed 34 assertions, including forced termination during recovery and successful recovery on a subsequent attempt. On this NTFS test volume, renaming the backup directory while the recovery file handles were held was blocked; this observation is not a guarantee for every filesystem. The installer recovery-loop fixture passed 23 assertions; the uninstall validators passed 78. Tests use fake registry data and did not invoke an actual installer or remove real product files. A forced process kill is not a power-loss test and is not proof that every filesystem crash state is recoverable.

R60.17 was assembled and its 19 release payloads, embedded ZIP, current scripts, TSF DLL and CLI hashes verified. Setup SHA-256 df0f2356dd7bc1c2f25922e35892f6b313642266339942219716195b23c590bc. Defender custom scanning without remediation found no threat; Google Drive classification is unknown. R60.17 has not been installed, uninstalled, or tested in Windows Search/Microsoft Store. The prior failed local transaction still blocks installation pending Windows UAC.

A simulated UI-less host now exercises candidate COM interface discovery, exact page-index retrieval, host-owned display, selection, finalization, update and end notifications. It passed, but does not launch Windows Search or Microsoft Store and cannot prove text entry in either application.


## 2026-10-28: isolated UI-less input using the packaged DLL

A repeatable isolated TSF fixture loads the actual R60.17 package DLL and its adjacent release dictionary, creates an in-process TSF document with TF_TMAE_NOACTIVATETIP and UI-element-only activation, and invokes the DLL's ITfKeyEventSink. The input sequences a + Space and oek + Space produced 對 and 嘸 in the TSF document. Settings are confined to the unique build fixture directory and removed by the test. The fixture did not register the TIP or activate other installed TIPs. A fresh read-only status afterwards showed all seven product resources absent; the original unresolved transaction still blocks installation.

This test exercises the shipped DLL, dictionary and isolated UI-less TSF editing, but it does not test Windows Search, Microsoft Store, app-container restrictions, actual user keyboard focus or a normally registered installation. Real install/uninstall and Search/Store acceptance remain outstanding.

The isolated packaged-DLL run is reproducible with native/tests/run-tsf-binary-uiless-tests.ps1; it validates the DLL and dictionary hashes against the release manifest before launching a unique fixture process. Authenticode inspection reports the R60.17 TSF DLL as NotSigned; this does not establish why Google Drive restricted an earlier installer.

## 2026-10-28: real install and blocked uninstall

The R60.12 failed transaction was manually archived after elevated checks and hash-verified backup; the original outcome was not rewritten. History inspection then returned success. R60.17 completed an interactive installation with its confirmation and reported payload cleanup. The installed uninstaller matches the installer SHA-256. Its real uninstall failed before registration changes because an exclusive open of the input-method DLL encountered a sharing violation; elevated readback confirmed the registration remained present. The uninstaller's temporary worker and extracted payload were cleaned up.

The mapped-module preflight now runs before exclusive file opens and again before registration removal, so a known loader is identified before the generic sharing violation. The isolated uninstall validators passed 81 checks. R60.18 was built with this change; its 19 manifest entries and embedded ZIP contents were checked. Setup SHA-256: a3fea781d5fb7b67b42a1fa86f932bd0f2b5ae76ec45dca23bfd10547c37783e. A real R60.18 uninstall attempt against the installed R60.17 stopped before registration changes and reported msrdc.exe PID 12188 as holding the DLL; elevated status confirmed registrations remained. The extracted temporary payload was cleaned. The occupied-process preflight is a point-in-time check and does not prevent subsequent loads. A successful real uninstall, reinstall, and Search/Store input test remain outstanding.

After the user explicitly authorized ending the identified msrdc.exe process, R60.18 successfully uninstalled the R60.17 installation. An independent elevated status showed all seven product resources absent, both installation directories absent, and history inspection succeeded. R60.18 was then installed interactively; all 12 installed package files matched the manifest, and the installed Uninstall.exe matched the R60.18 setup SHA-256. The user reported that both Windows taskbar Search and Microsoft Store produced the expected Chinese characters for `a` + Space and `oek` + Space. This is user-reported acceptance, not an automated captured test.

The installed R60.18 uninstaller was subsequently launched for its own end-to-end test. It correctly stopped before registration removal because Explorer, SearchHost, ChatGPT and Chrome had loaded the input-method DLL, and its temporary worker and payload were cleaned. A successful installed-R60.18-uninstaller run and a final reinstall remain pending release of those applications; the R60.18 installation remains present.

## R60.20 automatic failure diagnostics

Setup embeds the read-only collector as a separate resource and includes it in the release bundle. On an install/uninstall error it attempts to create a local diagnostic ZIP before removing the extracted payload, records the ZIP path or collection error in the setup log when writable, and reports the location in the non-silent error dialog. It also creates a fallback setup log for failures before normal argument parsing completes. Cancellation of the install confirmation remains a cancellation, not a collected failure. The collector adds diagnosis.json with observed lifecycle results and collection issue counts; it does not claim the error code proves a root cause. It does not upload the archive.

The Setup fixture passed 61 checks, including a simulated collector, argument quoting, collector failure and temporary script removal. A separate run exercised the real collector and verified the diagnosis summary. The built R60.20 executable was run with an intentionally invalid argument and --silent: it returned the expected error and produced a local ZIP containing the original error log, environment.json, collection-issues.json and diagnosis.json, without executing installation or uninstallation. All 20 release files and ZIP entries matched their manifests and source files. Setup SHA-256: ea593cf2f266754871f739f6e7ef58490222861b8ab2a820ea08d2f27b5fbab6. R60.20 has not undergone an actual install/uninstall cycle or a real child-installer failure test; collection can be partial when Windows denies access.

## 2026-10-29: installed uninstaller and R60.20 installation

The installed R60.18 uninstaller completed successfully after applications released the DLL; its elevated worker exited 0 and both worker and payload cleanup were logged. Independent elevated readback found all seven registration resources absent, no product or maintenance directory, and history inspection succeeded. R60.20 then completed interactive installation with exit 0 and payload cleanup. All 12 installed package files matched the R60.20 package manifest; the installed uninstaller matched the R60.20 setup SHA-256. Independent elevated readback reported the expected x64 COM registration, profile, categories, and user enabled. Actual Search/Store input confirmation for R60.20, a successful R60.20 uninstaller run, and a real child-installer failure diagnostic remain pending.

The user subsequently confirmed R60.20 produced the expected Chinese characters for both input sequences in taskbar Search and Microsoft Store. The installed R60.20 uninstaller then stopped on a real mapped-DLL preflight failure for ChatGPT and SearchHost, before modifying registration. The uninstaller generated a local diagnostics ZIP, and inspection confirmed that it contained the actual failure log, 31 transaction evidence files, diagnosis summary and zero reported collection issues; worker and payload cleanup completed. Independent elevated status confirmed R60.20 remained registered. A successful installed R60.20 uninstall and final reinstall remain pending release of the DLL.

After the user authorized closing the identified TourBox process, the installed R60.20 uninstaller completed successfully (exit 0); worker and payload cleanup were logged. Independent elevated status showed all seven product resources absent, profile registry absence, no product or maintenance directory, and history inspection succeeded. A subsequent R60.20 interactive reinstall was canceled at its confirmation dialog before deployment (exit 1223). The user explicitly chose to leave the product uninstalled. A successful R60.20 reinstallation after this removal is therefore not verified; the earlier R60.20 installation and Search/Store input results remain separately documented above.

## R60.21 long English composition

Normal composition no longer drops input after five characters, allowing Enter to commit the accumulated original keys. Dictionary lookups and the separate phonetic/homophone query limits retain their existing rules; normal candidate selection by number is unchanged. TSF text writes reject values that exceed the Windows LONG length representation instead of truncating on conversion.

The engine fixture verified 2,048 letters, case preservation in the engine, digit and auxiliary-key entry after a long code, Backspace, Escape, and Chinese candidate selection. An isolated TSF UI-less fixture loaded the actual R60.21 packaged DLL and dictionary, committed two Chinese characters, typed 600 English letters, removed one with Backspace and verified all 599 committed by Enter. One repeat failed at the first Chinese-character assertion and a subsequent repeat passed; the intermittent fixture failure's cause is unknown. This fixture does not test an installed IME, Windows Search or Microsoft Store. The package's 20 release payloads and embedded ZIP were hash-verified. Setup SHA-256: b8322f649330b5b0b4a3b8cf3976b30b722853ddeb0364c50c2f3c5b1f12b30f. The candidate has not been installed or tested interactively; the local machine remains uninstalled per user preference.
