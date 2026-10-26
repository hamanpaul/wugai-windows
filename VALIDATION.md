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
