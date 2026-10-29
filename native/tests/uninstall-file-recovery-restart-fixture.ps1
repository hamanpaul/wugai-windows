param([Parameter(Mandatory=$true)][string]$FixtureRoot,
      [Parameter(Mandatory=$true)][string]$ReceiptPath,
      [Parameter(Mandatory=$true)][string]$ReadyFile,
      [Parameter(Mandatory=$true)][string]$FixtureSid)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo 'build')).TrimEnd('\')
$script:fixtureRoot=[IO.Path]::GetFullPath($FixtureRoot)
$script:fixtureReceipt=[IO.Path]::GetFullPath($ReceiptPath)
$script:fixtureReady=[IO.Path]::GetFullPath($ReadyFile)
$script:fixtureSid=$FixtureSid
foreach($path in @($script:fixtureRoot,$script:fixtureReceipt,$script:fixtureReady,$env:LOCALAPPDATA)) {
    if(-not [IO.Path]::GetFullPath($path).StartsWith($build+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture path.' }
}
. (Join-Path $repo 'native\maintenance.ps1')
. (Join-Path $repo 'native\uninstall-file-recovery.ps1')
function Get-WuGaiCurrentSid { return $script:fixtureSid }
function Get-WuGaiRemovalReceiptPath([string]$InstallRoot) {
    if(-not $InstallRoot.Equals($script:fixtureRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture root changed.' }
    return $script:fixtureReceipt
}
function Read-WuGaiAppsEntry { return $null }
function Invoke-WuGaiUninstallCli {
    $names=@('com_x64','com_x86','profile','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')
    $resources=@($names | ForEach-Object {[pscustomobject]@{name=$_;read_result=0;present=$false;lifecycle_owned=$false}})
    return [pscustomobject]@{kind='windows-installer-lifecycle';operation=0;cleanup=0;readback=0;
        profile_present_result=0;profile_present=$false;profile_registry_absence_result=0;
        all_categories_result=0;all_categories_count=0;resources=$resources}
}
function Copy-Item([string]$LiteralPath,[string]$Destination) {
    $target=[IO.Path]::GetFullPath($Destination)
    if(-not $target.StartsWith($script:fixtureRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture copy target.' }
    Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $target
    [IO.File]::WriteAllText($script:fixtureReady,'one-file-restored')
    Start-Sleep -Seconds 30
}
Recover-WuGaiInterruptedFileRemoval $script:fixtureRoot $script:fixtureSid 'fixture-only'
