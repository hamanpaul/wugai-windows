param(
    [Parameter(Mandatory=$true)][string]$BundleRoot,
    [string]$InstallRoot = (Join-Path $env:ProgramFiles 'LiuKai'),
    [switch]$Execute,
    [string]$ExpectedUserSid = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'maintenance.ps1')
. (Join-Path $PSScriptRoot 'uninstall-file-recovery.ps1')




function ConvertFrom-WuGaiModuleProcessList([string[]]$Lines) {
    $seen = @{}
    foreach ($line in $Lines) {
        if (-not $line.StartsWith('"')) { continue } # Localized "no matching tasks" message.
        $row = $line | ConvertFrom-Csv -Header ImageName,ProcessId,Modules
        $processId = 0
        Assert-WuGai ([int]::TryParse($row.ProcessId,[ref]$processId) -and $processId -gt 0 -and
            -not [string]::IsNullOrWhiteSpace($row.ImageName)) 'Invalid process inventory.'
        if (-not $seen.ContainsKey($processId)) {
            $seen[$processId] = $true
            [pscustomobject]@{Name=$row.ImageName; Id=$processId}
        }
    }
}
function Get-WuGaiModuleProcessCandidates {
    $tasklist = Join-Path $env:SystemRoot 'System32\tasklist.exe'
    $lines = @(& $tasklist /m BoshiamyTsf.dll /fo csv /nh)
    Assert-WuGai ($LASTEXITCODE -eq 0) 'Could not inspect processes using the input method DLL.'
    ConvertFrom-WuGaiModuleProcessList $lines
}
function Get-WuGaiProcessModulePaths([int]$ProcessId) {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    try {
        foreach ($module in $process.Modules) {
            if ($module.ModuleName -ieq 'BoshiamyTsf.dll') { $module.FileName }
        }
    } finally { $process.Dispose() }
}
function Get-WuGaiMappedModuleUsers([string[]]$DllPaths) {
    Assert-WuGai ($DllPaths.Count -gt 0) 'No installed input method DLL was identified.'
    $targets = @{}
    foreach ($path in $DllPaths) {
        Assert-WuGai ([IO.Path]::IsPathRooted($path) -and [IO.Path]::GetFileName($path) -ieq 'BoshiamyTsf.dll') 'Invalid input method DLL path.'
        $targets[[IO.Path]::GetFullPath($path).ToLowerInvariant()] = $true
    }
    foreach ($candidate in @(Get-WuGaiModuleProcessCandidates)) {
        try {
            $observedPaths = @(Get-WuGaiProcessModulePaths $candidate.Id | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            if ($observedPaths.Count -eq 0) { throw 'The reported module was not visible during inspection.' }
            foreach ($path in $observedPaths) {
                if ($path -and $targets.ContainsKey([IO.Path]::GetFullPath($path).ToLowerInvariant())) {
                    [pscustomobject]@{Name=$candidate.Name; Id=$candidate.Id; Inspection='matched'}
                    break
                }
            }
        } catch {
            # A disappearing or inaccessible process must not be assumed safe.
            [pscustomobject]@{Name=$candidate.Name; Id=$candidate.Id; Inspection='unavailable'}
        }
    }
}
function Assert-WuGaiModuleNotMapped($Plan, [string]$Root) {
    $dllPaths = @($Plan.files | Where-Object { $_.relative_path -match '(^|/)BoshiamyTsf\.dll$' } |
        ForEach-Object { Join-Path $Root $_.relative_path.Replace('/','\') })
    $mappedUsers = @(Get-WuGaiMappedModuleUsers $dllPaths)
    if ($mappedUsers.Count) {
        $names = @($mappedUsers | ForEach-Object { $_.Name + ' (PID ' + $_.Id + ', ' + $_.Inspection + ')' }) -join ', '
        throw ('The input method DLL may still be loaded by: ' + $names + '. Close or restart these programs and retry; no registrations were removed.')
    }
}


$releaseMutex = $null
$releaseLockHeld = $false
$fileRemovalStarted = $false
$maintenanceBackup = $null
$removalReceipt = $null
try {
    $bundle = (Resolve-Path -LiteralPath $BundleRoot).Path
    Assert-WuGaiNoReparse $bundle -Tree
    $release = Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-WuGai ($release.format -ceq 'liukai-release-v1' -and $release.architecture -ceq 'x64' -and
        $release.registration_scope -ceq 'user-and-machine-com-x64-v1') 'Unsupported uninstall bundle.'
    foreach ($row in $release.files) {
        Assert-WuGai ($row.path -match '^[A-Za-z0-9._/-]+$' -and -not $row.path.Contains('..') -and -not $row.path.StartsWith('/')) 'Invalid release path.'
        $file = Join-Path $bundle $row.path.Replace('/','\')
        Assert-WuGai ((Get-WuGaiHash $file) -ceq $row.sha256 -and (Get-Item -LiteralPath $file).Length -eq $row.bytes) "Uninstall bundle mismatch: $($row.path)"
    }
    # Separate from the native engine's mutex: child CLIs must remain able to
    # acquire their own lock while this wrapper owns the complete release flow.
    $releaseMutex = New-Object Threading.Mutex($false,'Global\WuGai-ReleaseLifecycle-v1')
    try { $releaseLockHeld = $releaseMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $releaseLockHeld = $true }
    Assert-WuGai $releaseLockHeld 'Another WuGai installation or removal is in progress.'
    $sid = Get-WuGaiCurrentSid
    if ($ExpectedUserSid) { Assert-WuGai ($sid -ceq $ExpectedUserSid) 'Windows account changed during elevation.' }
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    $root = $paths.InstallRoot
    $cli = Join-Path $bundle 'package\LiuKaiInstaller.exe'
    Assert-WuGai (-not $cli.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase) -and
        -not $cli.StartsWith($paths.MaintenanceRoot + '\',[StringComparison]::OrdinalIgnoreCase)) 'Uninstall engine must run outside the directories being removed.'
    if ($Execute) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        try { $admin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
        finally { $identity.Dispose() }
        Assert-WuGai $admin 'Run the uninstaller as administrator using the original Windows account.'
        Recover-WuGaiMaintenancePublication -InstallRoot $root -ExpectedUserSid $sid
        Recover-WuGaiInterruptedFileRemoval -InstallRoot $root -ExpectedUserSid $sid -Cli $cli
    }
    $maintenance = Read-WuGaiMaintenance $root $sid
    $entry = ConvertTo-WuGaiEntry (Read-WuGaiAppsEntry $paths)
    Assert-WuGaiAppsEntry $paths $entry $sid
    Assert-WuGai (-not $entry -or $null -ne $maintenance) 'Uninstall entry has no owned maintenance payload.'
    $planned = Invoke-WuGaiUninstallCli $cli @('--plan-uninstall-package',$root)
    $plan = Assert-WuGaiUninstallReport $planned $root $sid $false
    Assert-WuGaiUninstallInventory $root $plan
    if (-not $Execute) { $planned | ConvertTo-Json -Depth 20; exit 0 }

    # Identify loaded input-method DLLs before opening files exclusively, so
    # the user can see which applications need to release the DLL.
    Assert-WuGaiModuleNotMapped $plan $root

    # All backups and logs remain outside the installation and maintenance trees.
    $backupBase = Join-Path $env:LOCALAPPDATA 'LiuKai\uninstall-backups'
    $backup = Join-Path $backupBase ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N'))
    Assert-WuGaiNoReparse $backup
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    [IO.File]::WriteAllText((Join-Path $backup 'plan.json'), ($planned | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
    foreach ($row in $plan.files) {
        $source = Join-Path $root $row.relative_path.Replace('/','\')
        $destination = Join-Path $backup ('package\' + $row.relative_path.Replace('/','\'))
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
        Assert-WuGai ((Get-WuGaiHash $destination) -ceq $row.sha256) 'Uninstall backup verification failed.'
        # Stop before registration changes if an application holds a payload open.
        $handle = [IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        $handle.Dispose()
    }
    if ($maintenance) {
        $maintenanceBackup = Join-Path $backup 'maintenance'
        $maintenanceManifestHash = Get-WuGaiHash (Join-Path $paths.MaintenanceRoot 'maintenance.json')
        [IO.Directory]::CreateDirectory($maintenanceBackup) | Out-Null
        foreach ($name in @('Uninstall.exe','maintenance.json')) {
            $source = Join-Path $paths.MaintenanceRoot $name
            Copy-Item -LiteralPath $source -Destination (Join-Path $maintenanceBackup $name)
            Assert-WuGai ((Get-WuGaiHash (Join-Path $maintenanceBackup $name)) -ceq (Get-WuGaiHash $source)) 'Maintenance backup mismatch.'
            $handle = [IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
            $handle.Dispose()
        }
    }
    # FileShare.None can succeed for an image mapped into a process, although
    # Windows will still reject its deletion. Check the exact installed DLL
    # paths before removing any registrations; the later rollback still guards
    # against an application loading a DLL after this observation.
    Assert-WuGaiModuleNotMapped $plan $root
    # Validate every recovery input and the receipt destination before any
    # registration is removed, so a bad backup cannot strand the user midway.
    $backupPlanHash = Get-WuGaiHash (Join-Path $backup 'plan.json')
    $null = Assert-WuGaiRemovalBackup $root $sid $backup $backupPlanHash
    if ($maintenance) {
        $backupImage = Get-WuGaiMaintenanceImage $paths $maintenanceBackup $sid
        Assert-WuGai ($backupImage.SetupHash -ceq $maintenance.setup_sha256 -and
            $backupImage.ManifestHash -ceq $maintenanceManifestHash) 'Maintenance backup changed before uninstall.'
    }
    $receiptPath = Get-WuGaiRemovalReceiptPath $root
    Assert-WuGaiNoReparse $receiptPath
    Assert-WuGai (-not (Test-Path -LiteralPath $receiptPath)) 'A prior file-removal recovery receipt still exists.'
    $removed = Invoke-WuGaiUninstallCli $cli @('--uninstall-package-registration',$root,'--diagnostic-trace') (Join-Path $backup 'registration.json')
    $proof = Assert-WuGaiUninstallReport $removed $root $sid $true
    Assert-WuGai ($proof.inventory_fingerprint -ceq $plan.inventory_fingerprint -and $proof.manifest_sha256 -ceq $plan.manifest_sha256) 'Installation changed during uninstall; program files retained.'
    $status = Invoke-WuGaiUninstallCli $cli @('--status','--machine-com-x64') (Join-Path $backup 'status-before-file-removal.json')
    Assert-WuGaiAbsentReport $status
    Assert-WuGaiUninstallInventory $root $plan
    # The protected receipt is durable before deletion starts. It binds the
    # already verified backup to this account and installation for recovery.
    $removalReceipt = Write-WuGaiRemovalReceipt -Root $root -Sid $sid -Backup $backup -AppsEntry $entry -Maintenance $maintenance `
        -ExpectedPlanHash $backupPlanHash -ExpectedMaintenanceManifestHash $maintenanceManifestHash
    # This exact absolute root was validated by the native owner/manifest reader
    # and the complete file/directory/hash inventory immediately above.
    $fileRemovalStarted = $true
    Remove-Item -LiteralPath $root -Recurse -Force
    Assert-WuGai (-not (Test-Path -LiteralPath $root)) 'Program file removal is incomplete; retain the uninstall entry and backup.'
    Remove-WuGaiMaintenance $root $sid
    $final = Invoke-WuGaiUninstallCli $cli @('--status','--machine-com-x64') (Join-Path $backup 'status-final.json')
    Assert-WuGaiAbsentReport $final
    Remove-WuGaiRemovalReceipt -Root $root -ExpectedHash $removalReceipt.Hash
    $removalReceipt = $null
    [pscustomobject]@{kind='wugai-package-uninstall'; complete=$true; install_root=$root; backup=$backup;
        settings_preserved=$true; transaction_history_preserved=$true} | ConvertTo-Json
    Write-Output 'Uninstallation complete. User settings, backups, and transaction history were preserved.'
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($fileRemovalStarted) {
        try {
            Restore-WuGaiUninstallFiles $root $plan (Join-Path $backup 'package')
            if ($maintenanceBackup) {
                Assert-WuGai ((Get-WuGaiHash (Join-Path $maintenanceBackup 'Uninstall.exe')) -ceq $maintenance.setup_sha256 -and
                    (Get-WuGaiHash (Join-Path $maintenanceBackup 'maintenance.json')) -ceq $maintenanceManifestHash) 'Maintenance backup changed after capture.'
                Restore-WuGaiMaintenanceBackup -InstallRoot $root -ExpectedUserSid $sid -BackupRoot $maintenanceBackup -AppsEntry $entry
            }
            if ($null -ne $removalReceipt) {
                Remove-WuGaiRemovalReceipt -Root $root -ExpectedHash $removalReceipt.Hash
                $removalReceipt = $null
            }
            [Console]::Error.WriteLine('Program files and the uninstall entry were restored for retry. Registrations remain removed. Backup: ' + $backup)
        } catch {
            [Console]::Error.WriteLine('Automatic file restore stopped: ' + $_.Exception.Message + '. Preserve the verified backup: ' + $backup)
        }
    }
    exit 1
} finally {
    if ($releaseLockHeld) { $releaseMutex.ReleaseMutex() }
    if ($null -ne $releaseMutex) { $releaseMutex.Dispose() }
}
