# Collect state for the legacy orphan guard; no writes or cleanup are performed.
. (Join-Path $PSScriptRoot 'legacy-owner-marker.ps1')
function Get-LegacyRecoveryState {
    param([Parameter(Mandatory=$true)][string]$Cli,
          [Parameter(Mandatory=$true)][string]$HistoryProbe,
          [Parameter(Mandatory=$true)][string]$ExpectedUserSid)
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal=New-Object Security.Principal.WindowsPrincipal($identity)
        $sid=$identity.User.Value
        $statusText=& $Cli '--status' '--machine-com-x64' 2>&1 | Out-String
        $statusExit=$LASTEXITCODE
        $status=$statusText | ConvertFrom-Json
        $historyText=& $HistoryProbe 2>&1 | Out-String
        $historyExit=$LASTEXITCODE
        $history=$historyText | ConvertFrom-Json
        if($history.kind -cne 'liukai-history-preflight-v1'){throw 'Unrecognized transaction history report.'}
        $installRoot=[IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'LiuKai')).TrimEnd('\')
        $sha=[Security.Cryptography.SHA256]::Create()
        try {$rootHash=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($installRoot.ToLowerInvariant()))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
        $removalReceipt=Join-Path (Join-Path $env:ProgramFiles '.wugai-uninstall-file-recovery') ('WuGai-'+$rootHash+'.json')
        $publicationReceipt=$installRoot+'.maintenance.publish.json'
        $profilePath='Software\Microsoft\CTF\TIP\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\LanguageProfile\0x00000404\{7254A21F-6EB1-4217-A42C-C27301175D90}'
        $markerPath='Software\LiuKai\Installer\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\Profile'
        $hkcu=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
        try {
            $profile=$hkcu.OpenSubKey($profilePath,$false);$marker=$hkcu.OpenSubKey($markerPath,$false)
            try {
                if($null -eq $profile -or $null -eq $marker){throw 'Expected user profile or ownership marker missing.'}
                $profileNames=@($profile.GetValueNames());$markerNames=@($marker.GetValueNames())
                if($profileNames.Count -ne 1 -or $markerNames.Count -ne 1){throw 'Unexpected registry value count.'}
                $profileValue=$profile.GetValue('Enable',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $markerValue=$marker.GetValue('Record',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                if($markerValue -isnot [byte[]]){throw 'Invalid owner marker.'}
                $owner=Read-LegacyOwnerMarker $markerValue
                return [pscustomobject]@{
                    IsElevated=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
                    UserSid=$sid;ExpectedUserSid=$ExpectedUserSid;HistoryExit=$historyExit;HistoryInspection=$history.machine_history;LegacyHistoryInspection=$history.legacy_history
                    PendingFileRemoval=(Test-Path -LiteralPath $removalReceipt);PendingMaintenancePublication=(Test-Path -LiteralPath $publicationReceipt)
                    InstallRootExists=(Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'LiuKai'))
                    MaintenanceRootExists=(Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'LiuKai.maintenance'))
                    StatusExit=$statusExit;Status=$status;ProfileSubkeys=$profile.SubKeyCount;ProfileValueCount=$profileNames.Count
                    ProfileValueName=$profileNames[0];ProfileValueKind=$profile.GetValueKind('Enable').ToString();ProfileEnable=$profileValue
                    MarkerSubkeys=$marker.SubKeyCount;MarkerValueCount=$markerNames.Count;MarkerValueName=$markerNames[0]
                    MarkerValueKind=$marker.GetValueKind('Record').ToString();MarkerOwner=$owner.Owner;MarkerIcon=$owner.Icon
                    MarkerEnabled=$owner.Enabled;MarkerEncodingValid=$true;MarkerBytes=[Convert]::ToBase64String($markerValue)
                    ExpectedIcon=(Join-Path $env:ProgramFiles 'LiuKai\shared\branding.dll')
                }
            } finally {if($profile){$profile.Dispose()};if($marker){$marker.Dispose()}}
        } finally {$hkcu.Dispose()}
    } finally {$identity.Dispose()}
}

# Read-only state for interrupted-backup restoration; unlike initial recovery,
# either user leaf may already be absent. Never creates a registry key.
function Get-LegacyResumeState {
    param([Parameter(Mandatory=$true)][string]$Cli,[Parameter(Mandatory=$true)][string]$HistoryProbe,
          [Parameter(Mandatory=$true)][string]$ExpectedUserSid)
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    try {
        $principal=New-Object Security.Principal.WindowsPrincipal($identity)
        $statusText=& $Cli '--status' '--machine-com-x64' 2>&1 | Out-String;$statusExit=$LASTEXITCODE
        $status=$statusText|ConvertFrom-Json
        $historyText=& $HistoryProbe 2>&1 | Out-String;$historyExit=$LASTEXITCODE
        $history=$historyText|ConvertFrom-Json
        if($history.kind -cne 'liukai-history-preflight-v1'){throw 'Unrecognized transaction history report.'}
        $installRoot=[IO.Path]::GetFullPath((Join-Path $env:ProgramFiles 'LiuKai')).TrimEnd('\')
        $sha=[Security.Cryptography.SHA256]::Create()
        try {$rootHash=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($installRoot.ToLowerInvariant()))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
        $removalReceipt=Join-Path (Join-Path $env:ProgramFiles '.wugai-uninstall-file-recovery') ('WuGai-'+$rootHash+'.json')
        $publicationReceipt=$installRoot+'.maintenance.publish.json'
        $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
        try {
            $path='Software\Microsoft\CTF\TIP\{5D7C4291-2B34-4CE8-8A11-39627105C42E}\LanguageProfile\0x00000404\{7254A21F-6EB1-4217-A42C-C27301175D90}'
            $key=$machine.OpenSubKey($path,$false)
            try {$machineExists=$null -ne $key} finally {if($key){$key.Dispose()}}
        } finally {$machine.Dispose()}
        [pscustomobject]@{
            IsElevated=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
            UserSid=$identity.User.Value;ExpectedUserSid=$ExpectedUserSid
            HistoryExit=$historyExit;HistoryInspection=$history.machine_history;LegacyHistoryInspection=$history.legacy_history
            PendingFileRemoval=(Test-Path -LiteralPath $removalReceipt);PendingMaintenancePublication=(Test-Path -LiteralPath $publicationReceipt)
            InstallRootExists=(Test-Path -LiteralPath $installRoot);MaintenanceRootExists=(Test-Path -LiteralPath ($installRoot+'.maintenance'))
            MachineProfileExists=$machineExists;StatusExit=$statusExit;Status=$status
        }
    } finally {$identity.Dispose()}
}
