param([Parameter(Mandatory=$true)][string]$WixExe,
      [Parameter(Mandatory=$true)][string]$Msi,
      [Parameter(Mandatory=$true)][string]$SourceBundle,
      [Parameter(Mandatory=$true)][string]$HistoryProbe,
      [Parameter(Mandatory=$true)][string]$ActionDll,
      [Parameter(Mandatory=$true)][string]$ExtractDir)
$ErrorActionPreference='Stop'
$wix=(Resolve-Path -LiteralPath $WixExe).Path;$msiPath=(Resolve-Path -LiteralPath $Msi).Path
$source=(Resolve-Path -LiteralPath $SourceBundle).Path;$extract=[IO.Path]::GetFullPath($ExtractDir)
if(Test-Path -LiteralPath $extract){throw 'MSI extraction directory already exists.'}
$package=Join-Path $source 'package';$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$expected=@{}
foreach($row in $manifest.files){
    $relative=[string]$row.path
    if($relative -cnotmatch '^[A-Za-z0-9._/-]+$' -or $relative.Contains('..') -or $relative.StartsWith('/')){throw 'Unsafe package path.'}
    $name=[IO.Path]::GetFileName($relative);if($expected.ContainsKey($name)){throw 'Duplicate payload name.'}
    $file=Join-Path $package $relative.Replace('/','\')
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256 -or (Get-Item -LiteralPath $file).Length -ne $row.bytes){throw "Source mismatch: $relative"}
    $expected[$name]=$row.sha256
}
$register=Join-Path $source 'LiuKaiRegister.exe';$expected['LiuKaiRegister.exe']=(Get-FileHash -LiteralPath $register -Algorithm SHA256).Hash.ToLowerInvariant()
$expected['package-manifest.json']=(Get-FileHash -LiteralPath (Join-Path $package 'manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
foreach($name in @('recover-legacy-profile.ps1','legacy-prepared-root.ps1','legacy-recovery-policy.ps1','legacy-recovery-observe.ps1','legacy-recovery-transaction.ps1','legacy-recovery-adapter.ps1','legacy-recovery-backup.ps1','legacy-recovery-registry.ps1','legacy-owner-marker.ps1')) {
    $expected[$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$expected['legacy-history-probe.exe']=(Get-FileHash -LiteralPath $HistoryProbe -Algorithm SHA256).Hash.ToLowerInvariant()
$expected['collect-install-diagnostics.ps1']=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '..\collect-install-diagnostics.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.Directory]::CreateDirectory($extract)|Out-Null
& $wix msi decompile $msiPath -o (Join-Path $extract 'decompiled.wxs') -x (Join-Path $extract 'payload')
if($LASTEXITCODE -ne 0){throw 'MSI decompilation failed.'}
$binarySources=@{
    UserContext=$ActionDll
    LegacyRecovery=(Join-Path $PSScriptRoot 'recover-legacy-profile.ps1')
    LegacyRecoveryPreparedRoot=(Join-Path $PSScriptRoot 'legacy-prepared-root.ps1')
    LegacyRecoveryPolicy=(Join-Path $PSScriptRoot 'legacy-recovery-policy.ps1')
    LegacyRecoveryObserve=(Join-Path $PSScriptRoot 'legacy-recovery-observe.ps1')
    LegacyRecoveryTransaction=(Join-Path $PSScriptRoot 'legacy-recovery-transaction.ps1')
    LegacyRecoveryAdapter=(Join-Path $PSScriptRoot 'legacy-recovery-adapter.ps1')
    LegacyRecoveryBackup=(Join-Path $PSScriptRoot 'legacy-recovery-backup.ps1')
    LegacyRecoveryRegistry=(Join-Path $PSScriptRoot 'legacy-recovery-registry.ps1')
    LegacyRecoveryOwner=(Join-Path $PSScriptRoot 'legacy-owner-marker.ps1')
    LegacyHistoryProbe=$HistoryProbe
    FailureDiagnostics=(Join-Path $PSScriptRoot '..\collect-install-diagnostics.ps1')
}
$binaryDir=Join-Path $extract 'payload\Binary'
$binaryFiles=@(Get-ChildItem -LiteralPath $binaryDir -File)
if($binaryFiles.Count -ne $binarySources.Count){throw 'Unexpected MSI binary resource count.'}
foreach($name in $binarySources.Keys){
    $embedded=Join-Path $binaryDir $name
    if(-not (Test-Path -LiteralPath $embedded -PathType Leaf) -or (Get-FileHash -LiteralPath $embedded -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $binarySources[$name] -Algorithm SHA256).Hash){throw "MSI binary resource mismatch: $name"}
}
$db=(New-Object -ComObject WindowsInstaller.Installer).OpenDatabase($msiPath,0)
$view=$db.OpenView('SELECT * FROM `File`');$view.Execute();$seen=@{};$count=0
while($row=$view.Fetch()){
    $id=$row.StringData(1);$parts=$row.StringData(3).Split('|');$name=$parts[$parts.Length-1]
    if($id -cnotmatch '^[A-Za-z_][A-Za-z0-9_.]*$' -or -not $expected.ContainsKey($name) -or $seen.ContainsKey($name)){throw "Unexpected MSI file entry: $name"}
    $file=Join-Path (Join-Path $extract 'payload\File') $id
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected[$name]){throw "MSI payload hash mismatch: $name"}
    $seen[$name]=$true;$count++
}
if($count -ne $expected.Count){throw 'MSI payload is incomplete.'}
[pscustomobject]@{msi_payload_files_verified=$count;source_hashes_match=$true;binary_resources_verified=$binarySources.Count}|ConvertTo-Json
