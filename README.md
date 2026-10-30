# 嘸．改 Windows x64

發行建置、測試、診斷與驗收流程見 [RELEASE_FLOW.md](RELEASE_FLOW.md)。

原生 Windows TSF 輸入法，使用 Android 手 key 字表與無改圖示。字碼以原字根表為準；品牌保持彩色，另有獨立中／英狀態。不包含自訂字詞、關聯詞或品牌灰階切換。

R60（`0.0.0-dev.20261026.r60.machine-com-x64`）已於 2026-10-26 完成本輪使用者實機驗收：Windows Search、Microsoft Store 的 `a + Space`／`oek + Space` 中文輸入，以及獨立中／英模式圖示，使用者確認「三項都正常」。安裝後 12 個套件檔案雜湊與 HKCU64／HKLM64 雙 scope 的獨立驗後均通過。

[下載 R60 安裝程式](https://github.com/hamanpaul/wugai-windows/releases/download/r60/WuGai-R60-x64-Setup.exe) · [SHA256](https://github.com/hamanpaul/wugai-windows/releases/download/r60/SHA256SUMS.txt) · [發行說明](https://github.com/hamanpaul/wugai-windows/releases/tag/r60)

已知安裝問題：上述 R60 的已安裝狀態驗收不等於新電腦安裝驗收。R60 首次註冊的圖示讀回缺陷已修正；另一台 R60.1 的 `0x8007000D` 仍待完整交易紀錄確認。此分支另加入正式卸載入口及診斷，驗證範圍見 [VALIDATION.md](VALIDATION.md)。

Terminal 沿用先前正常結果，本輪沒有重新測試。不宣稱完整 77 項 backlog 或官版功能 1:1 全部完成。驗收範圍見 [VALIDATION.md](VALIDATION.md)。

R60 新增明確的 x64 機器層級 COM 註冊，與使用者層級一起處理升級、修復、回復與解除。相同 SID、安裝目錄與所有權才允許寫入。保留舊交易格式與記錄，不改寫歷史。

本次為 **Windows x64 prerelease**，tag 為 `r60`；安裝檔為 `WuGai-R60-x64-Setup.exe`。安裝必須以同一 Windows 帳號完成管理員授權。此來源不含官版程式、逆向輸出、本機部署記錄、登入憑證或私人來源路徑。

## 同音字查詢（開發中）

在中文輸入模式先按 `'`，接著輸入已知同音字的字碼，按空白選擇該字，顯示同音候選字；以空白鍵翻頁、數字鍵選字。例：`'`、`mnfo`、空白、`0` 可由「瞎」查找並輸出「蝦」。不同字的讀音和候選字以隨附字典資料為準。
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

## 解除安裝（新版安裝器）

R60.20 起，安裝或移除發生錯誤時，安裝程式會嘗試自動收集 WuGai 安裝紀錄、交易及相關狀態，在 `%LOCALAPPDATA%\LiuKai\diagnostics` 產生診斷 ZIP，並在錯誤視窗與安裝 LOG 中記錄路徑。診斷資料留在本機，不會自動上傳；可能包含 Windows 帳號路徑。若收集失敗且 LOG 可寫入，會記錄原始錯誤與收集失敗原因。取消安裝確認不視為錯誤。

安裝及獨立讀回成功後，安裝器會在 Windows「設定 → 應用程式 → 已安裝的應用程式」加入 **WuGai** 的解除安裝入口。卸載器跟隨安裝檔一起提供，不需要另下載腳本。也可直接執行同版 Setup：

```powershell
.\WuGai-R60.2-x64-Setup.exe --uninstall
```

自訂的既有安裝位置可用 `--uninstall --install-root "C:\完整路徑\LiuKai"`。請使用原安裝帳號完成 UAC 授權；開始前關閉正在使用輸入法的程式。`--silent` 只隱藏結果視窗，不略過 UAC。

卸載器先確認套件、帳號、所有權及交易歷史，備份完整檔案，再移除輸入法與 COM 註冊、程式檔案及 Windows 應用程式入口。程式檔案若被占用、含未知檔案，或存在無法驗證的未完成交易，會保留現況並記錄失敗；不會直接刪除整個資料夾來掩蓋問題。

維護檔放在安裝目錄旁的 `<安裝目錄>.maintenance`，包含 `Uninstall.exe` 及 `maintenance.json`。使用者設定不刪除；LOG 在 `%LOCALAPPDATA%\LiuKai\installer-logs`，卸載備份在 `%LOCALAPPDATA%\LiuKai\uninstall-backups`，原交易歷史也保留。

此開發分支在刪檔前保存恢復紀錄。若刪檔途中程序被終止，下一次以原帳號執行新版安裝或解除安裝時，會先核對紀錄與備份，再恢復檔案及原有入口；若資料已改動或狀態無法確認，會停止並保留紀錄。此流程目前只完成隔離測試，尚未完成實機卸載或斷電驗證。R60 和 R60.2 的既有發布檔不包含這項新功能。

此功能不會將舊的失敗交易標成成功，也不會自動抹除歷史來強行重裝。尚未完成乾淨 Windows 的安裝／卸載／重裝實機驗收。

## 測試與來源範圍

`native/tests/` 保留本輪 machine COM、交易／回復、安裝腳本讀回及中英狀態的既有測試來源。部分整合測試需要另外提供受控的完整套件 fixture；私人／官版 fixture 不隨 repo 發布。選用的歷史 baseline 比對標頭亦未附帶。這份匯出未重新執行測試，不將來源測試視為實機驗收。

五個產品入口及其 quoted-include 依賴由來源 commit `2fea4b8` 匯出。產品 C++ 原樣保留，僅兩份開發腳本的本機編譯器預設改為通用位置；差異記於 `SOURCE-MANIFEST.json`。七份資料來自實際 R60 套件，phonetic provenance 已清除私人路徑。第三方來源與聲明見 `THIRD_PARTY_NOTICES.md`。
