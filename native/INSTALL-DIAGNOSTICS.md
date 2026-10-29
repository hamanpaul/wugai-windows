# 安裝失敗紀錄收集

請在**安裝失敗的那台電腦**，使用原本安裝時的 Windows 帳號執行
`collect-install-diagnostics.ps1`。不需要重新執行安裝器。

將腳本存到一個可寫入的資料夾，在該資料夾開啟 PowerShell：

```powershell
powershell.exe -NoProfile -File .\collect-install-diagnostics.ps1
```

如安裝檔不在 Downloads，可明確指定，腳本只會讀取檔案資訊及雜湊：

```powershell
powershell.exe -NoProfile -File .\collect-install-diagnostics.ps1 -InstallerPath "D:\Downloads\WuGai-R60.1-x64-Setup.exe"
```

腳本會在目前資料夾建立 `WuGai-diagnostics-日期-識別碼.zip`，包含：

- 最近 10 份 WuGai 安裝 LOG。
- WuGai 的交易 journal / snapshot 與安裝狀態檔。
- 最近 5 個解壓暫存目錄中的 release / package manifest。
- 安裝程式與已安裝 CLI 的版本、SHA-256、簽章狀態。
- WuGai 專屬的 COM / TSF / owner marker 登錄值，以及 Windows build。
- 最近兩天中，內容提到 WuGai / LiuKai / BoshiamyTsf 的 Defender 偵測或處理事件（如有）。

它不會執行安裝、修復、解除安裝、修改登錄或刪除原始紀錄，也不會自動上傳。
ZIP 可能包含原始紀錄中的帳號路徑。若有讀取失敗，原因會列在 `collection-issues.json`。
未出現 Defender 事件不代表安全掃描通過。

若系統原有的腳本政策阻擋執行，可直接提供以下原始檔案，無須變更政策：

```text
%LOCALAPPDATA%\LiuKai\installer-logs\最新的 .log
C:\Program Files\.liukai-installer-ledger-machine-com-x64\LiuKai-TSF-v1\<交易識別碼>\transaction.journal
C:\Program Files\.liukai-installer-ledger-machine-com-x64\LiuKai-TSF-v1\<交易識別碼>\transaction.snapshot
```

請保留來源檔案，將 ZIP 或上述檔案複製到原本分享紀錄的位置。
Google Drive 警示文字需另外提供；本腳本無法取得 Google 的偵測結果。
