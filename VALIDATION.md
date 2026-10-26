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
