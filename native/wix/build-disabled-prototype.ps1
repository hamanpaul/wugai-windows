param(
    [Parameter(Mandatory=$true)][string]$WixExe,
    [Parameter(Mandatory=$true)][string]$BalExtension,
    [Parameter(Mandatory=$true)][string]$SourceBundle,
    [Parameter(Mandatory=$true)][string]$OutputDir,
    [Parameter(Mandatory=$true)][string]$ToolchainBin,
    [string]$MsiVersion='64.0.0'
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -lt 7){throw 'PowerShell 7 is required.'}
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$wix=(Resolve-Path -LiteralPath $WixExe).Path
$bal=(Resolve-Path -LiteralPath $BalExtension).Path
$wixVersion=(& $wix --version | Out-String).Trim()
if($LASTEXITCODE -ne 0 -or $wixVersion -cnotmatch '^6\.0\.2\+'){throw 'WiX 6.0.2 is required for this prototype.'}
$source=(Resolve-Path -LiteralPath $SourceBundle).Path
$out=[IO.Path]::GetFullPath($OutputDir)
if(Test-Path -LiteralPath $out){throw 'Output directory already exists.'}
if(-not (Test-Path -LiteralPath (Split-Path -Parent $out) -PathType Container)){throw 'Output parent must exist.'}
if($MsiVersion -cnotmatch '^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,5})$' -or [int]$Matches[1] -gt 255 -or [int]$Matches[2] -gt 255 -or [int]$Matches[3] -gt 65535){throw 'MSI version must use the supported major.minor.build ranges.'}
$release=Get-Content -LiteralPath (Join-Path $source 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($release.format -cne 'liukai-release-v1' -or $release.architecture -cne 'x64' -or $release.registration_scope -cne 'user-and-machine-com-x64-v1'){throw 'Invalid source release metadata.'}
$seen=@{}
foreach($row in $release.files){
    if($row.path -cnotmatch '^[A-Za-z0-9._/-]+$' -or $row.path.Contains('..') -or $row.path.StartsWith('/') -or $seen.ContainsKey($row.path)){throw 'Unsafe or duplicate source path.'}
    $seen[$row.path]=$true;$file=Join-Path $source $row.path.Replace('/','\')
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256 -or (Get-Item -LiteralPath $file).Length -ne $row.bytes){throw "Source manifest mismatch: $($row.path)"}
}
if(-not $seen.ContainsKey('package/manifest.json') -or -not $seen.ContainsKey('package/LiuKaiInstaller.exe') -or -not $seen.ContainsKey('LiuKaiRegister.exe')){throw 'Release manifest does not bind required MSI inputs.'}
$package=Join-Path $source 'package';$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8|ConvertFrom-Json
$version=[string]$manifest.version
if($version -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or $version.Contains('..') -or $version -cne $release.version){throw 'Invalid runtime version.'}
$runtime=Join-Path $package ('versions\'+$version+'\x64')
[IO.Directory]::CreateDirectory($out)|Out-Null
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$contextDll=Join-Path $out 'user-context.dll'
$manifestHash=(Get-FileHash -LiteralPath (Join-Path $package 'manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
$helperHash=(Get-FileHash -LiteralPath (Join-Path $package 'LiuKaiInstaller.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
$tsfHash=(Get-FileHash -LiteralPath (Join-Path $runtime 'BoshiamyTsf.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
$brandingHash=(Get-FileHash -LiteralPath (Join-Path $package 'shared\branding.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
$diagnosticsHash=(Get-FileHash -LiteralPath (Join-Path $repo 'native\collect-install-diagnostics.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
$historyProbe=Join-Path $out 'legacy-history-probe.exe'
& $compiler -std=c++17 -O2 -static (Join-Path $PSScriptRoot 'legacy-history-probe.cpp') -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -o $historyProbe
if($LASTEXITCODE -ne 0){throw 'Legacy history probe compilation failed.'}
$extraProof=[ordered]@{}
$extraProof['LiuKaiRegister.exe']=(Get-FileHash -LiteralPath (Join-Path $source 'LiuKaiRegister.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
foreach($name in @('recover-legacy-profile.ps1','legacy-prepared-root.ps1','legacy-recovery-policy.ps1','legacy-recovery-observe.ps1','legacy-recovery-transaction.ps1','legacy-recovery-adapter.ps1','legacy-recovery-backup.ps1','legacy-recovery-registry.ps1','legacy-owner-marker.ps1')){
    $extraProof['support/'+$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$extraProof['support/legacy-history-probe.exe']=(Get-FileHash -LiteralPath $historyProbe -Algorithm SHA256).Hash.ToLowerInvariant()
$extraProof['support/collect-install-diagnostics.ps1']=$diagnosticsHash
$proofText=(@($extraProof.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value}) -join ';')
$binding="#pragma once`ninline constexpr char kWixRuntimeVersion[] = `"$version`";`ninline constexpr char kWixManifestSha256[] = `"$manifestHash`";`ninline constexpr char kWixHelperSha256[] = `"$helperHash`";`ninline constexpr char kWixTsfSha256[] = `"$tsfHash`";`ninline constexpr char kWixBrandingSha256[] = `"$brandingHash`";`ninline constexpr char kWixDiagnosticsSha256[] = `"$diagnosticsHash`";`ninline constexpr char kWixHistorySha256[] = `"$($extraProof['support/legacy-history-probe.exe'])`";`ninline constexpr char kWixRecoveryProof[] = `"$proofText`";`n"
[IO.File]::WriteAllText((Join-Path $out 'wix-binding.hpp'),$binding,[Text.Encoding]::ASCII)
& $compiler -std=c++17 -O2 -static -shared "-I$out" (Join-Path $PSScriptRoot 'user-context.cpp') (Join-Path $PSScriptRoot 'registration-action.cpp') -lmsi -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -lshell32 -lwtsapi32 -o $contextDll
if($LASTEXITCODE -ne 0){throw 'User context action compilation failed.'}
$msi=Join-Path $out 'LiuKai-development-disabled.msi';$setup=Join-Path $out 'LiuKai-win-setup.exe';$icon=Join-Path $repo 'assets\liukai.ico'
& $wix build -arch x64 -d ("MsiVersion=$MsiVersion") -d ("PayloadRoot=$package") -d ("RegistrationTool=$(Join-Path $source 'LiuKaiRegister.exe')") -d ("RuntimeVersion=$version") -d ("RuntimeRoot=$runtime") -d ("UserContextDll=$contextDll") -d ("SupportRoot=$PSScriptRoot") -d ("LegacyHistoryProbe=$historyProbe") -d ("DiagnosticsScript=$(Join-Path $repo 'native\collect-install-diagnostics.ps1')") -d ("ManifestSha256=$manifestHash") -o $msi (Join-Path $PSScriptRoot 'package.wxs')
if($LASTEXITCODE -ne 0){throw 'Prototype MSI build failed.'}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'native\tests\wix-product-layout-tests.ps1') -Msi $msi
if($LASTEXITCODE -ne 0){throw 'Prototype MSI layout check failed.'}
& (Join-Path $PSScriptRoot 'verify-msi-payload.ps1') -WixExe $wix -Msi $msi -SourceBundle $source -HistoryProbe $historyProbe -ActionDll $contextDll -ExtractDir (Join-Path $out 'msi-verification')
& $wix build -arch x64 -ext $bal -d ("BundleVersion=$MsiVersion") -d ("IconPath=$icon") -d ("LicensePath=$(Join-Path $PSScriptRoot 'install-notice.rtf')") -d ("LogoPath=$(Join-Path $repo 'assets\liukai-installer-logo.png')") -d ("MsiPath=$msi") -o $setup (Join-Path $PSScriptRoot 'bundle.wxs')
if($LASTEXITCODE -ne 0){throw 'Prototype Burn build failed.'}
& (Join-Path $PSScriptRoot 'verify-bundle.ps1') -WixExe $wix -Bundle $setup -Msi $msi -Icon $icon -Logo (Join-Path $repo 'assets\liukai-installer-logo.png') -Notice (Join-Path $PSScriptRoot 'install-notice.rtf') -ExtractDir (Join-Path $out 'verification-extracted')
[IO.File]::WriteAllText((Join-Path $out 'DO-NOT-INSTALL.txt'),"Development prototype only. The MSI intentionally refuses installation; TSF lifecycle and rollback are not implemented.`r`n")
[pscustomobject]@{kind='wix-disabled-prototype';runtime_version=$version;msi=$msi;setup=$setup;installable=$false}|ConvertTo-Json
