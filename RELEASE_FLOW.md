# Windows x64 release flow

> **WiX 遷移狀態**：目前工作樹正將發行流程改為建置 `LiuKai.msi` 和 `LiuKai-win-setup.exe`。下方舊的 `WuGai-Rxx-x64-Setup.exe` 建置指令與診斷 ZIP 說明僅適用舊安裝器，不能用來驗證 WiX 包。WiX 包的來源建置、MSI／EXE、logo 與安裝說明核對由 `native/wix/build-installer.ps1` 執行；安裝失敗的 Windows Installer 日誌預設位於 `%TEMP%`，符合條件的 MSI 失敗會嘗試自動收集 ZIP；可驗證的孤立舊版登錄殘留會先備份再清理，其餘情況會停止。啟動程式失敗、禁用回復或收集程序本身失敗時，可能無法建立 ZIP。正式發行前需完成實機驗證。

## 原則

- 發行內容從指定 Git commit、受版控的程式碼和 `assets/` 完整重建；不得把舊 release 的 DLL、工具或字表默默混入新包。記錄 commit、編譯器、版本及 SHA-256；雜湊供核對，不代表程式安全或已簽章。`SOURCE-MANIFEST.json` 是 R60 原始來源匯出的歷史紀錄，不代表新版本。
- 保留使用者設定與交易證據。解除安裝前驗證帳號、所有權、檔案清單與註冊；DLL 正被載入、讀回不確定或失敗交易無法證明已回復時停止，不能直接刪除登錄鍵或交易目錄。
- 使用獨立讀回確認結果，保留安裝及卸載 LOG。失敗時自動嘗試收集本機診斷 ZIP，記錄收集缺漏；不自動上傳。診斷檔可能含帳號路徑及登錄內容，分享前應確認對象。
- 預發布版本先完成實機及另一台電腦的安裝與卸載驗收，再評估正式發布。Google Drive 可能依其政策限制檔案；未簽章與 Defender 結果不足以判定其原因。若遭限制，依 Google 官方流程申請審查，不以重新封裝方式繞過。

## 建置及自動發行

1. 檢查工作樹、差異與來源權利，執行 `git diff --check`。Windows 需要 PowerShell 7、Windows PowerShell 5.1、.NET Framework csc、x64 MinGW g++ / windres / ld；工具鏈必須回報 `x86_64-w64-mingw32`。
2. 使用獨立且尚不存在的目錄執行：

   ```powershell
   powershell.exe -NoProfile -File .\native\test-release.ps1 -ToolchainBin C:\msys64\mingw64\bin -WorkDir "$PWD\build\release-tests"
   pwsh -NoProfile -File .\native\build-fresh-release.ps1 -ToolchainBin C:\msys64\mingw64\bin -Version '0.0.0-dev.20261029.r61.x64' -OutputDir "$PWD\dist\release" -SetupName 'WuGai-R61-x64-Setup.exe'
   powershell.exe -NoProfile -File .\native\verify-release.ps1 -ReleaseRoot "$PWD\dist\release" -SetupName 'WuGai-R61-x64-Setup.exe'
   ```

   每次使用新目錄；失敗目錄保留供檢查。測試使用隔離 fixture，不代表真實安裝驗收。從原始碼重建不保證不同比較環境產生逐位元相同的 PE。
3. 提交並推送來源後，建立並推送唯一 `r<number>` tag。GitHub Actions 會在 Windows 執行上述測試與完整建置、核對外層 EXE 的嵌入資源及所有 payload 雜湊，產生帶 `SOURCE.txt`、`SHA256SUMS.txt` 的 GitHub prerelease。Workflow 失敗不會自動發布；不要覆蓋既有 tag 或檔案來掩蓋失敗。
4. 從 GitHub Release 重新下載 EXE 與 SHA256SUMS，核對下載檔案雜湊和 release tag 所指 commit；不以本機建置品代替下載測試。

## 實機驗收與故障處理

在同一 Windows 帳號且已備份工作的環境，記錄原始安裝狀態，先清理舊版再安裝下載檔；完成 UAC 與安裝前說明。驗證安裝檔案、HKCU/HKLM 註冊、輸入法啟用與 Search／Store 中文輸入、長英文 Enter 輸出。透過已安裝的卸載程式移除，確認註冊與程式／維護目錄消失、使用者設定保留、暫存工作程式清理，然後依使用者希望保留安裝或卸載狀態。

若失敗，停止重試並保留 `%LOCALAPPDATA%\LiuKai\installer-logs`、診斷 ZIP 與 transaction journal / snapshot。確認 ZIP 中 `collection-issues.json` 的缺漏及 `diagnosis.json` 的觀察值；這份摘要不是根因判定。跨電腦 R60.1 未完成交易需先核對實際狀態與原始交易、備份及持有鎖，不能直接套用另一台的封存腳本。DLL 被占用時先告知程序名稱，讓使用者處理可能有未保存工作的應用程式。
