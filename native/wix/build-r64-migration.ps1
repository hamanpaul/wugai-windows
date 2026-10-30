param(
 [Parameter(Mandatory=$true)][string]$WixExe,
 [Parameter(Mandatory=$true)][string]$R64Msi,
 [Parameter(Mandatory=$true)][string]$ToolchainBin,
 [Parameter(Mandatory=$true)][string]$WorkDir
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -lt 7){throw 'PowerShell 7 is required.'}
$original=(Resolve-Path -LiteralPath $R64Msi).Path
$expected='a5be4848ae645562473936bdd7c0bb1ae44bb8a3371c140cbe19321834b4a51e'
if((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected){throw 'R64 MSI is not the pinned GitHub release.'}
$work=[IO.Path]::GetFullPath($WorkDir);if(Test-Path -LiteralPath $work){throw 'Migration work directory already exists.'}
[void][IO.Directory]::CreateDirectory($work)
$wix=(Resolve-Path -LiteralPath $WixExe).Path
$compiler=Join-Path (Resolve-Path -LiteralPath $ToolchainBin).Path 'g++.exe'
$decompiled=Join-Path $work 'decompiled.wxs';$payload=Join-Path $work 'payload'
& $wix msi decompile $original -o $decompiled -x $payload
if($LASTEXITCODE -ne 0){throw 'R64 MSI extraction failed.'}
[xml]$xml=Get-Content -LiteralPath $decompiled -Raw -Encoding UTF8
$files=@{}
foreach($node in $xml.SelectNodes("//*[local-name()='File']")){
 $name=$node.GetAttribute('Name');$id=$node.GetAttribute('Id')
 if($name){if($files.ContainsKey($name) -or $id -notmatch '^[A-Za-z0-9_.]+$'){throw 'Unexpected R64 file mapping.'};$files[$name]=Join-Path $payload ('File\'+$id)}
}
function HashFile([string]$name){
 if(-not $files.ContainsKey($name) -or -not (Test-Path -LiteralPath $files[$name] -PathType Leaf)){throw "Missing R64 payload: $name"}
 return (Get-FileHash -LiteralPath $files[$name] -Algorithm SHA256).Hash.ToLowerInvariant()
}
$proof=[ordered]@{};$proof['LiuKaiRegister.exe']=HashFile 'LiuKaiRegister.exe'
foreach($name in @('recover-legacy-profile.ps1','legacy-prepared-root.ps1','legacy-recovery-policy.ps1','legacy-recovery-observe.ps1','legacy-recovery-transaction.ps1','legacy-recovery-adapter.ps1','legacy-recovery-backup.ps1','legacy-recovery-registry.ps1','legacy-owner-marker.ps1','legacy-history-probe.exe','collect-install-diagnostics.ps1')){$proof['support/'+$name]=HashFile $name}
$proofText=(@($proof.GetEnumerator()|ForEach-Object {$_.Key+'='+$_.Value}) -join ';')
$binding=@"
#pragma once
inline constexpr char kWixRuntimeVersion[] = "0.0.0-dev.20261030.r64.x64";
inline constexpr char kWixManifestSha256[] = "$(HashFile 'package-manifest.json')";
inline constexpr char kWixHelperSha256[] = "$(HashFile 'LiuKaiInstaller.exe')";
inline constexpr char kWixTsfSha256[] = "$(HashFile 'BoshiamyTsf.dll')";
inline constexpr char kWixBrandingSha256[] = "$(HashFile 'branding.dll')";
inline constexpr char kWixDiagnosticsSha256[] = "$(HashFile 'collect-install-diagnostics.ps1')";
inline constexpr char kWixHistorySha256[] = "$(HashFile 'legacy-history-probe.exe')";
inline constexpr char kWixRecoveryProof[] = "$proofText";
"@
[IO.File]::WriteAllText((Join-Path $work 'wix-binding.hpp'),$binding,[Text.Encoding]::ASCII)
$context=Join-Path $work 'patched-context.dll'
& $compiler -std=c++17 -O2 -static -shared "-I$work" (Join-Path $PSScriptRoot 'user-context.cpp') (Join-Path $PSScriptRoot 'registration-action.cpp') -lmsi -lole32 -loleaut32 -luuid -ladvapi32 -lbcrypt -lshlwapi -lshell32 -lwtsapi32 -o $context
if($LASTEXITCODE -ne 0){throw 'R64 migration action compilation failed.'}
$base=Join-Path $work 'r64-original.msi';$updated=Join-Path $work 'r64-patched.msi'
Copy-Item -LiteralPath $original -Destination $base;Copy-Item -LiteralPath $original -Destination $updated
$installer=New-Object -ComObject WindowsInstaller.Installer
$db=$installer.OpenDatabase($updated,1)
$condition='REMOVE="ALL"'
$sql=@(
 "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('MSIRESTARTMANAGERCONTROL', 'DisableShutdown')",
 "INSERT INTO ``Registry`` (``Registry``, ``Root``, ``Key``, ``Name``, ``Value``, ``Component_``) VALUES ('LiuKaiSafeRetire', 2, 'Software\LiuKai\MSI\{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}', 'SafeRetire', '1', 'InstallIdentity')",
 "INSERT INTO ``LaunchCondition`` (``Condition``, ``Description``) VALUES ('NOT RollbackDisabled', 'Windows Installer rollback must be enabled.')",
 "UPDATE ``InstallExecuteSequence`` SET ``Sequence`` = 2593 WHERE ``Action`` = 'CollectRemovalFailure'",
 "UPDATE ``InstallExecuteSequence`` SET ``Sequence`` = 2594 WHERE ``Action`` = 'UndoRemoval'",
 "UPDATE ``InstallExecuteSequence`` SET ``Sequence`` = 2595 WHERE ``Action`` = 'RemoveRegistration'",
 "INSERT INTO ``CustomAction`` (``Action``, ``Type``, ``Source``, ``Target``) VALUES ('UndoRetireFiles', 17665, 'UserContext', 'UndoRetireFiles')",
 "INSERT INTO ``CustomAction`` (``Action``, ``Type``, ``Source``, ``Target``) VALUES ('RetireFiles', 17409, 'UserContext', 'RetireFiles')",
 "INSERT INTO ``CustomAction`` (``Action``, ``Type``, ``Source``, ``Target``) VALUES ('CommitRetireFiles', 17921, 'UserContext', 'CommitRetireFiles')",
 "INSERT INTO ``InstallExecuteSequence`` (``Action``, ``Condition``, ``Sequence``) VALUES ('UndoRetireFiles', '$condition', 2596)",
 "INSERT INTO ``InstallExecuteSequence`` (``Action``, ``Condition``, ``Sequence``) VALUES ('RetireFiles', '$condition', 2597)",
 "INSERT INTO ``InstallExecuteSequence`` (``Action``, ``Condition``, ``Sequence``) VALUES ('CommitRetireFiles', '$condition', 2598)"
)
foreach($statement in $sql){$view=$db.OpenView($statement);$view.Execute()}
$view=$db.OpenView("UPDATE ``Binary`` SET ``Data`` = ? WHERE ``Name`` = 'UserContext'")
$record=$installer.CreateRecord(1);$record.SetStream(1,$context);$view.Execute($record);$db.Commit()
$view=$null;$record=$null;$db=$null;[GC]::Collect();[GC]::WaitForPendingFinalizers()
$layout=Join-Path $PSScriptRoot '..\tests\wix-product-layout-tests.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $layout -Msi $updated -Installable -LegacyMigration
if($LASTEXITCODE -ne 0){throw 'Patched R64 MSI safety validation failed.'}
$patch=Join-Path $work 'r64-migration.msp'
& $wix build -bindpath $work -out $patch (Join-Path $PSScriptRoot 'r64-migration-patch.wxs')
if($LASTEXITCODE -ne 0){throw 'R64 migration patch build failed.'}
Write-Output $patch
