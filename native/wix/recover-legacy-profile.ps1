param(
    [Parameter(Mandatory=$true)][string]$Cli,
    [Parameter(Mandatory=$true)][string]$CliSha256,
    [Parameter(Mandatory=$true)][string]$HistoryProbe,
    [Parameter(Mandatory=$true)][string]$HistorySha256,
    [Parameter(Mandatory=$true)][string]$ExpectedUserSid,
    [switch]$Execute,
    [string]$ResumeReceipt = '',
    [string]$PreparedVersion = '',
    [string]$PreparedManifestSha256 = '',
    [string]$PreparedExtraHashes = ''
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'legacy-prepared-root.ps1')
. (Join-Path $PSScriptRoot 'legacy-recovery-policy.ps1')
. (Join-Path $PSScriptRoot 'legacy-recovery-observe.ps1')
. (Join-Path $PSScriptRoot 'legacy-recovery-transaction.ps1')
. (Join-Path $PSScriptRoot 'legacy-recovery-adapter.ps1')
function Assert-ProbeIdentity([string]$Path,[string]$Expected) {
    if($Expected -cnotmatch '^[0-9a-f]{64}$' -or -not (Test-Path -LiteralPath $Path -PathType Leaf) -or
       (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Expected) {throw 'A required diagnostic executable failed integrity verification.'}
}
function Assert-DirectoryChain([string]$Path) {
    $item=Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if(-not $item.PSIsContainer){throw 'Expected a directory.'}
    while($null -ne $item) {
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Directory path contains a reparse point.'}
        $parent=[IO.Directory]::GetParent($item.FullName);if($null -eq $parent){break}
        $item=Get-Item -LiteralPath $parent.FullName -Force -ErrorAction Stop
    }
}
Assert-ProbeIdentity $Cli $CliSha256
Assert-ProbeIdentity $HistoryProbe $HistorySha256
$profileParent='Software\Microsoft\CTF\TIP\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\LanguageProfile\0x00000404'
$markerParent='Software\LiuKai\Installer\{5D7C4291-2B34-4CE8-8A11-39627105C42E}'
$preparedProof=$null
if($PreparedVersion -or $PreparedManifestSha256 -or $PreparedExtraHashes){
    if(-not $PreparedVersion -or -not $PreparedManifestSha256 -or -not $PreparedExtraHashes){throw 'Prepared MSI proof is incomplete.'}
    $root=[IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'LiuKai')).TrimEnd('\')
    $extra=ConvertFrom-PreparedMsiExtraHashes $PreparedExtraHashes
    if(-not [IO.Path]::GetFullPath($Cli).Equals((Join-Path $root 'LiuKaiInstaller.exe'),[StringComparison]::OrdinalIgnoreCase) -or
       -not [IO.Path]::GetFullPath($HistoryProbe).Equals((Join-Path $root 'support\legacy-history-probe.exe'),[StringComparison]::OrdinalIgnoreCase) -or
       $extra['support/legacy-history-probe.exe'] -cne $HistorySha256){throw 'Prepared MSI helper identity mismatch.'}
    $preparedProof={ Assert-PreparedMsiRoot -Root $root -Version $PreparedVersion -ManifestSha256 $PreparedManifestSha256 -ExtraHashes $extra }.GetNewClosure()
}
$observer={
    $state=Get-LegacyRecoveryState -Cli $Cli -HistoryProbe $HistoryProbe -ExpectedUserSid $ExpectedUserSid
    if($preparedProof){if(-not (& $preparedProof)){throw 'Prepared MSI root could not be verified.'};$state.InstallRootExists=$false}
    return $state
}.GetNewClosure()
if(-not $ResumeReceipt) {
    $initial=& $observer
    Assert-LegacyProfileRecoveryPreconditions $initial
    if(-not $Execute){[pscustomobject]@{kind='liukai-legacy-recovery-plan';user_sid=$initial.UserSid;verified_preconditions=$true;executed=$false}|ConvertTo-Json;exit 0}
} elseif(-not $Execute){throw 'Explicit execution is required to restore a receipt.'}
$locks=New-Object 'System.Collections.Generic.List[System.Threading.Mutex]'
$hkcu=$null;$profile=$null;$marker=$null
try {
    foreach($name in @('Global\WuGai-ReleaseLifecycle-v1','Global\LiuKai-7254A21F-ProfileDisplayMetadata')) {
        $mutex=New-Object Threading.Mutex($false,$name)
        try {if(-not $mutex.WaitOne(0)){throw 'A related lifecycle operation is running.'}} catch [Threading.AbandonedMutexException] {$mutex.Dispose();throw 'A prior lifecycle operation was interrupted; preserve its evidence.'}
        $locks.Add($mutex)
    }
    $local=[IO.Path]::GetFullPath($env:LOCALAPPDATA);Assert-DirectoryChain $local
    $app=Join-Path $local 'LiuKai';if(-not (Test-Path -LiteralPath $app)){[IO.Directory]::CreateDirectory($app)|Out-Null};Assert-DirectoryChain $app
    $backup=Join-Path $app 'legacy-recovery-backups';if(-not (Test-Path -LiteralPath $backup)){[IO.Directory]::CreateDirectory($backup)|Out-Null};Assert-DirectoryChain $backup

    $hkcu=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    $profile=$hkcu.OpenSubKey($profileParent,$true);$marker=$hkcu.OpenSubKey($markerParent,$true)
    if($null -eq $profile -or $null -eq $marker){throw 'Verified registry parent is unavailable.'}
    $postcheck={
        $reportText=& $Cli '--status' '--machine-com-x64' 2>&1 | Out-String
        if($LASTEXITCODE -ne 0){return $false}
        $report=$reportText|ConvertFrom-Json
        if($report.kind -cne 'windows-installer-lifecycle' -or $report.scope -cne 'user-and-machine-com-x64' -or
           $report.readback -ne 0 -or $report.profile_registry_absence_result -ne 0 -or
           $report.profile_present -ne $false -or $report.all_categories_result -ne 0 -or $report.all_categories_count -ne 0){return $false}
        $resources=@($report.resources);if($resources.Count -ne 7){return $false}
        foreach($item in $resources){if($item.read_result -ne 0 -or $item.present -ne $false -or $item.lifecycle_owned -ne $false){return $false}}
        $historyText=& $HistoryProbe 2>&1 | Out-String;if($LASTEXITCODE -ne 0){return $false}
        $history=$historyText|ConvertFrom-Json
        return $history.kind -ceq 'liukai-history-preflight-v1' -and $history.machine_history -eq 0 -and $history.legacy_history -eq 0
    }.GetNewClosure()
    $resumeScope={param($document)
        $current=Get-LegacyResumeState -Cli $Cli -HistoryProbe $HistoryProbe -ExpectedUserSid $ExpectedUserSid
        if($preparedProof){if(-not (& $preparedProof)){throw 'Prepared MSI root could not be verified.'};$current.InstallRootExists=$false}
        Assert-LegacyResumePreconditions $current
        return $document.Snapshot.UserSid -ceq $ExpectedUserSid -and $document.Snapshot.ExpectedUserSid -ceq $ExpectedUserSid
    }.GetNewClosure()
    $store=New-LegacyRecoveryStore -ProfileParent $profile -MarkerParent $marker -BackupRoot $backup -Observer $observer -Postcheck $postcheck -ResumeScope $resumeScope
    $pending=@(Get-LegacyRecoveryPendingReceipts -Root $backup -Store $store)
    if($ResumeReceipt) {
        $requested=[IO.Path]::GetFullPath($ResumeReceipt)
        $selected=@($pending | Where-Object {$_.Path.Equals($requested,[StringComparison]::OrdinalIgnoreCase)})
        if($selected.Count -ne 1){throw 'The requested receipt is not a verified pending backup in the recovery directory.'}
        $result=Resume-LegacyRecoveryTransaction $store $selected[0]
        [pscustomobject]@{kind='liukai-legacy-recovery-resume';user_sid=$ExpectedUserSid;result=$result;backup=$selected[0].Path}|ConvertTo-Json
    } else {
        if($pending.Count -ne 0){throw 'An interrupted recovery requires explicit restoration before another cleanup.'}
        $receipt=Invoke-LegacyRecoveryTransaction $store
        [pscustomobject]@{kind='liukai-legacy-recovery';user_sid=$ExpectedUserSid;complete=$true;backup=$receipt.Path;sha256=$receipt.Sha256}|ConvertTo-Json
    }
} finally {
    if($marker){$marker.Dispose()};if($profile){$profile.Dispose()};if($hkcu){$hkcu.Dispose()}
    for($i=$locks.Count-1;$i -ge 0;$i--){$locks[$i].ReleaseMutex();$locks[$i].Dispose()}
}
