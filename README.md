# 嘸．改 Windows x64

原生 Windows TSF 輸入法，使用 Android 手 key 字表與無改圖示。字碼以原字根表為準；品牌保持彩色，另有獨立中／英狀態。不包含自訂字詞、關聯詞或品牌灰階切換。

R60（`0.0.0-dev.20261026.r60.machine-com-x64`）已於 2026-10-26 完成本輪使用者實機驗收：Windows Search、Microsoft Store 的 `a + Space`／`oek + Space` 中文輸入，以及獨立中／英模式圖示，使用者確認「三項都正常」。安裝後 12 個套件檔案雜湊與 HKCU64／HKLM64 雙 scope 的獨立驗後均通過。

[下載 R60 安裝程式](https://github.com/hamanpaul/wugai-windows/releases/download/r60/WuGai-R60-x64-Setup.exe) · [SHA256](https://github.com/hamanpaul/wugai-windows/releases/download/r60/SHA256SUMS.txt) · [發行說明](https://github.com/hamanpaul/wugai-windows/releases/tag/r60)

Terminal 沿用先前正常結果，本輪沒有重新測試。不宣稱完整 77 項 backlog 或官版功能 1:1 全部完成。驗收範圍見 [VALIDATION.md](VALIDATION.md)。

R60 新增明確的 x64 機器層級 COM 註冊，與使用者層級一起處理升級、修復、回復與解除。相同 SID、安裝目錄與所有權才允許寫入。保留舊交易格式與記錄，不改寫歷史。

本次為 **Windows x64 prerelease**，tag 為 `r60`；安裝檔為 `WuGai-R60-x64-Setup.exe`。安裝必須以同一 Windows 帳號完成管理員授權。此來源不含官版程式、逆向輸出、本機部署記錄、登入憑證或私人來源路徑。

## 原始碼建置

需要 Windows x64、MSYS2 MinGW-w64 x64 C++/windres、Python 3.10 以上及 PowerShell 7。以下從 repo 根目錄執行；編譯入口與連結參數沿用既有產品建置。此匯出未另做一次完整建置，不保證與已打包版本逐位元相同。R60 安裝包沿用已驗 R59 的服務 DLL、工具、字表及品牌；新的 CLI、註冊工具與安裝外殼則重新建置。

```powershell
New-Item -ItemType Directory -Force build/x64 | Out-Null
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
python native/toolchain.py --architecture x64 --check
& C:\msys64\mingw64\bin\windres.exe -I assets native/tsf/resources.rc -O coff -F pe-x86-64 -o build/x64/resources.o
python native/toolchain.py --architecture x64 -- -std=c++17 -shared -static -municode native/tsf/service.cpp native/tsf/service.def build/x64/resources.o -lole32 -loleaut32 -luuid -lgdi32 -o build/x64/BoshiamyTsf.dll
python native/toolchain.py --architecture x64 -- -std=c++17 -static -municode -mwindows native/tools/lookup_main.cpp build/x64/resources.o -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -o build/x64/LiuKaiLookup.exe
python native/toolchain.py --architecture x64 -- -std=c++17 -static -municode -mwindows native/tools/settings_main.cpp build/x64/resources.o -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -o build/x64/LiuKaiSettings.exe
python native/toolchain.py --architecture x64 -- -std=c++17 -static -municode native/tools/register_main.cpp -lole32 -loleaut32 -luuid -ladvapi32 -o build/x64/LiuKaiRegister.exe
python native/toolchain.py --architecture x64 -- -std=c++17 -static -municode native/installer/lifecycle_main.cpp -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o build/x64/LiuKaiInstaller.exe
Get-ChildItem assets -File | Where-Object { $_.Extension -in @('.tsv','.json','.md') } | Copy-Item -Destination build/x64
pwsh -NoProfile -File native/build-branding.ps1 -Architecture x64 -OutputDll build/x64/branding.dll
```

`native/package-windows.ps1` 可將這些輸出與 `assets/` 建立 x64 offline package；必須提供 `-X64Only`、版本與輸出目錄。`native/package/Setup.cs` 是 .NET Framework 安裝外殼，`install-release.ps1` 需要完整的 release manifest 與註冊工具，不能直接當成單一獨立安裝檔執行。沒有附帶需保留歷史套件的本機增量 builder，也沒有附帶已消耗的部署續行工具。

## 測試與來源範圍

`native/tests/` 保留本輪 machine COM、交易／回復、安裝腳本讀回及中英狀態的既有測試來源。部分整合測試需要另外提供受控的完整套件 fixture；私人／官版 fixture 不隨 repo 發布。選用的歷史 baseline 比對標頭亦未附帶。這份匯出未重新執行測試，不將來源測試視為實機驗收。

五個產品入口及其 quoted-include 依賴由來源 commit `2fea4b8` 匯出。產品 C++ 原樣保留，僅兩份開發腳本的本機編譯器預設改為通用位置；差異記於 `SOURCE-MANIFEST.json`。七份資料來自實際 R60 套件，phonetic provenance 已清除私人路徑。第三方來源與聲明見 `THIRD_PARTY_NOTICES.md`。
