# Shared validation and file-restoration helpers for install and uninstall.
# Dot-source maintenance.ps1 before loading this file.

function Assert-WuGaiUninstallReport($Report, [string]$Root, [string]$Sid, [bool]$Executed) {
    Assert-WuGai ($Report.kind -ceq 'windows-installer-lifecycle' -and $Report.operation -eq 0 -and
        $Report.cleanup -eq 0 -and $Report.readback -eq 0) 'Uninstall preflight/registration failed. Preserve the log and transaction history.'
    $proof = $Report.package_uninstall
    Assert-WuGai ($null -ne $proof -and $proof.schema_version -eq 1 -and $proof.executed -eq $Executed -and
        $proof.install_root -ceq $Root -and $proof.user_sid -ceq $Sid -and
        $proof.inventory_fingerprint -match '^[0-9a-f]{64}$' -and $proof.manifest_sha256 -match '^[0-9a-f]{64}$' -and
        @($proof.files).Count -gt 0 -and ((-not $Executed) -or $proof.registrations_absent -eq $true)) 'Uninstall proof mismatch.'
    return $proof
}

function Assert-WuGaiUninstallInventory([string]$Root, $Proof) {
    $paths = Get-WuGaiMaintenancePaths $Root
    Assert-WuGai ($paths.InstallRoot -ceq $Proof.install_root -and (Test-Path -LiteralPath $Root -PathType Container)) 'Uninstall root changed.'
    Assert-WuGaiNoReparse $Root -Tree
    $expected = @{}
    $expectedDirectories = @{}
    foreach ($row in $Proof.files) {
        $relative = [string]$row.relative_path
        Assert-WuGai ($relative -match '^[A-Za-z0-9._/-]+$' -and -not $relative.Contains('..') -and
            -not $relative.StartsWith('/') -and -not $relative.EndsWith('/') -and -not $relative.Contains('//') -and
            $row.sha256 -match '^[0-9a-f]{64}$' -and $row.bytes -ge 0 -and -not $expected.ContainsKey($relative)) 'Invalid uninstall inventory.'
        $expected[$relative] = $row
        $directory = [IO.Path]::GetDirectoryName($relative.Replace('/','\'))
        while ($directory) {
            $expectedDirectories[$directory.Replace('\','/')] = $true
            $directory = [IO.Path]::GetDirectoryName($directory)
        }
    }
    [string[]]$orderedPaths = @($expected.Keys)
    [Array]::Sort($orderedPaths, [StringComparer]::Ordinal)
    $image = New-Object Text.StringBuilder
    [void]$image.Append("liukai-uninstall-inventory-v1`n")
    foreach ($relative in $orderedPaths) { [void]$image.Append($relative + "`t" + $expected[$relative].sha256 + "`n") }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $fingerprint = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($image.ToString()))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
    Assert-WuGai ($fingerprint -ceq $Proof.inventory_fingerprint) 'Uninstall inventory fingerprint mismatch.'
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -Force -File)
    Assert-WuGai ($files.Count -eq $expected.Count) 'Installed files changed; nothing will be removed.'
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($Root.Length + 1).Replace('\','/')
        Assert-WuGai ($expected.ContainsKey($relative) -and $file.Length -eq $expected[$relative].bytes -and
            (Get-WuGaiHash $file.FullName) -ceq $expected[$relative].sha256) "Installed file changed: $relative"
    }
    foreach ($directory in Get-ChildItem -LiteralPath $Root -Recurse -Force -Directory) {
        $relative = $directory.FullName.Substring($Root.Length + 1).Replace('\','/')
        Assert-WuGai ($expectedDirectories.ContainsKey($relative)) "Unknown installation directory: $relative"
    }
}

function Assert-WuGaiAbsentReport($Report) {
    Assert-WuGai ($Report.kind -ceq 'windows-installer-lifecycle' -and $Report.operation -eq 0 -and $Report.cleanup -eq 0 -and
        $Report.readback -eq 0 -and $Report.profile_present_result -eq 0 -and $Report.profile_present -eq $false -and
        $Report.profile_registry_absence_result -eq 0 -and $Report.all_categories_result -eq 0 -and $Report.all_categories_count -eq 0 -and
        @($Report.resources).Count -eq 7) 'Independent uninstall readback failed; program files retained.'
    $names = @('com_x64','com_x86','profile','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')
    $seen = @{}
    foreach ($resource in $Report.resources) {
        Assert-WuGai ($resource.name -cin $names -and -not $seen.ContainsKey($resource.name) -and
            $resource.read_result -eq 0 -and $resource.present -eq $false -and $resource.lifecycle_owned -eq $false) 'Registration is unknown or still exists; program files retained.'
        $seen[$resource.name] = $true
    }
}

function Invoke-WuGaiUninstallCli([string]$Cli, [string[]]$Arguments, [string]$LogPath = '') {
    $text = & $Cli @Arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($LogPath) { [IO.File]::WriteAllText($LogPath, $text, (New-Object Text.UTF8Encoding($false))) }
    Write-Verbose $text
    if ($code -ne 0) {
        [Console]::Error.WriteLine($text)
        throw 'The uninstall engine stopped. Preserve its diagnostic output and backup.'
    }
    return ($text | ConvertFrom-Json)
}

# Remove-Item deletes whole files, so its failure can be retried by restoring
# missing files from the complete, verified backup. Never replace changed files
# or follow a junction while restoring an interrupted removal.

function Restore-WuGaiUninstallFiles([string]$Root, $Proof, [string]$BackupRoot) {
    $canonical = (Get-WuGaiMaintenancePaths $Root).InstallRoot
    Assert-WuGai ($canonical -ceq $Proof.install_root) 'Restore root mismatch.'
    $saved = $Proof | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $saved.install_root = [IO.Path]::GetFullPath($BackupRoot)
    Assert-WuGaiUninstallInventory $saved.install_root $saved
    Assert-WuGaiNoReparse $canonical -Tree
    $expected = @{}
    $directories = @{}
    foreach ($row in $Proof.files) {
        $expected[$row.relative_path] = $row
        $directory = [IO.Path]::GetDirectoryName($row.relative_path.Replace('/','\'))
        while ($directory) {
            $directories[$directory.Replace('\','/')] = $true
            $directory = [IO.Path]::GetDirectoryName($directory)
        }
    }
    if (Test-Path -LiteralPath $canonical) {
        Assert-WuGai (Test-Path -LiteralPath $canonical -PathType Container) 'Restore target is not a directory.'
        foreach ($item in Get-ChildItem -LiteralPath $canonical -Recurse -Force) {
            $relative = $item.FullName.Substring($canonical.Length+1).Replace('\','/')
            if ($item.PSIsContainer) { Assert-WuGai ($directories.ContainsKey($relative)) 'Unknown directory blocks restore.' }
            else {
                Assert-WuGai ($expected.ContainsKey($relative) -and $item.Length -eq $expected[$relative].bytes -and
                    (Get-WuGaiHash $item.FullName) -ceq $expected[$relative].sha256) 'Changed file blocks restore.'
            }
        }
    }
    [IO.Directory]::CreateDirectory($canonical) | Out-Null
    foreach ($row in $Proof.files) {
        $relative = $row.relative_path.Replace('/','\')
        $target = Join-Path $canonical $relative
        if (-not (Test-Path -LiteralPath $target)) {
            [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
            Copy-Item -LiteralPath (Join-Path $saved.install_root $relative) -Destination $target
        }
    }
    Assert-WuGaiUninstallInventory $canonical $Proof
}

# The receipt is placed in Program Files, rather than next to the per-user
# backup. Its protected copy binds the backup to the account, exact root, and
# immutable plan hash. Only the backup contents named by that plan are used.
function Get-WuGaiRemovalReceiptPath([string]$InstallRoot) {
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    $leaf = Split-Path -Leaf $paths.RegistryPath
    Assert-WuGai ($leaf -cmatch '^WuGai-[0-9a-f]{64}$') 'Invalid removal recovery identity.'
    $programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
    Assert-WuGai ([IO.Path]::IsPathRooted($programFiles)) 'Program Files path is unavailable.'
    return (Join-Path (Join-Path $programFiles '.wugai-uninstall-file-recovery') ($leaf+'.json'))
}
function Assert-WuGaiRemovalBackup([string]$Root, [string]$Sid, [string]$Backup, [string]$PlanHash) {
    Assert-WuGai ($PlanHash -cmatch '^[0-9a-f]{64}$') 'Invalid removal plan identity.'
    $base = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'LiuKai\uninstall-backups')).TrimEnd('\')
    $full = [IO.Path]::GetFullPath($Backup).TrimEnd('\')
    $parent = Split-Path -Parent $full; $leaf = Split-Path -Leaf $full
    Assert-WuGai ($full -ceq $Backup.TrimEnd('\') -and
        $parent.Equals($base,[StringComparison]::OrdinalIgnoreCase) -and
        $leaf -cmatch '^[0-9]{8}T[0-9]{6}Z-[0-9a-f]{32}$') 'Invalid uninstall backup location.'
    Assert-WuGaiNoReparse $full -Tree
    $planPath = Join-Path $full 'plan.json'
    Assert-WuGai ((Get-WuGaiHash $planPath) -ceq $PlanHash) 'Uninstall backup plan changed.'
    $planned = Get-Content -LiteralPath $planPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $proof = Assert-WuGaiUninstallReport $planned $Root $Sid $false
    $saved = $proof | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $saved.install_root = [IO.Path]::GetFullPath((Join-Path $full 'package'))
    Assert-WuGaiUninstallInventory $saved.install_root $saved
    return [pscustomobject]@{Backup=$full; Proof=$proof}
}
function Write-WuGaiRemovalReceipt([string]$Root, [string]$Sid, [string]$Backup, $AppsEntry, $Maintenance,
                                  [string]$ExpectedPlanHash, [string]$ExpectedMaintenanceManifestHash = '') {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $Sid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $Root
    $planHash = $ExpectedPlanHash
    $saved = Assert-WuGaiRemovalBackup $paths.InstallRoot $Sid $Backup $planHash
    $entry = ConvertTo-WuGaiEntry $AppsEntry
    Assert-WuGaiAppsEntry $paths $entry $Sid
    $hasMaintenance = $null -ne $Maintenance
    $setupHash = ''; $manifestHash = ''
    if ($hasMaintenance) {
        $image = Get-WuGaiMaintenanceImage $paths (Join-Path $saved.Backup 'maintenance') $Sid
        Assert-WuGai ($ExpectedMaintenanceManifestHash -cmatch '^[0-9a-f]{64}$' -and
            $image.Record.version -ceq $Maintenance.version -and $image.SetupHash -ceq $Maintenance.setup_sha256 -and
            $image.ManifestHash -ceq $ExpectedMaintenanceManifestHash) 'Maintenance backup differs from the owned uninstaller.'
        if ($entry) { Assert-WuGai ($entry.DisplayVersion -ceq $image.Record.version) 'Uninstall entry version differs from its backup.' }
        $setupHash = $image.SetupHash; $manifestHash = $image.ManifestHash
    } else { Assert-WuGai ($null -eq $entry -and -not $ExpectedMaintenanceManifestHash) 'Uninstall entry has no maintenance backup.' }
    $receipt = [ordered]@{format='wugai-uninstall-file-recovery-v1'; owner=$script:WuGaiOwner;
        install_root=$paths.InstallRoot; user_sid=$Sid; backup=$saved.Backup; plan_sha256=$planHash;
        maintenance_present=$hasMaintenance; maintenance_setup_sha256=$setupHash;
        maintenance_manifest_sha256=$manifestHash; apps_entry=$entry}
    $receiptPath = Get-WuGaiRemovalReceiptPath $paths.InstallRoot
    Assert-WuGaiNoReparse $receiptPath
    [IO.Directory]::CreateDirectory((Split-Path -Parent $receiptPath)) | Out-Null
    # A crash while serializing must not leave a partial final receipt. The
    # temporary file is flushed and then renamed within the protected folder;
    # deletion of installed files starts only after the rename succeeds.
    $temporary = $receiptPath + '.pending-' + [Guid]::NewGuid().ToString('N')
    Assert-WuGaiNoReparse $temporary
    try {
        Write-WuGaiPublicationReceipt $temporary $receipt
        [IO.File]::Move($temporary,$receiptPath)
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Assert-WuGaiNoReparse $temporary
            [IO.File]::Delete($temporary)
        }
    }
    return [pscustomobject]@{Path=$receiptPath; Hash=(Get-WuGaiHash $receiptPath)}
}
function Remove-WuGaiRemovalReceipt([string]$Root, [string]$ExpectedHash) {
    $path = Get-WuGaiRemovalReceiptPath $Root
    Assert-WuGaiNoReparse $path
    Assert-WuGai ($ExpectedHash -cmatch '^[0-9a-f]{64}$' -and (Get-WuGaiHash $path) -ceq $ExpectedHash) 'Removal recovery receipt changed.'
    [IO.File]::Delete($path)
    Assert-WuGai (-not (Test-Path -LiteralPath $path)) 'Removal recovery receipt could not be removed.'
}
function Recover-WuGaiInterruptedFileRemoval([string]$InstallRoot, [string]$ExpectedUserSid, [string]$Cli) {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $ExpectedUserSid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    $receiptPath = Get-WuGaiRemovalReceiptPath $paths.InstallRoot
    if (-not (Test-Path -LiteralPath $receiptPath)) { return }
    Assert-WuGaiNoReparse $receiptPath
    $receiptHash = Get-WuGaiHash $receiptPath
    $receipt = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-WuGai ($receipt.format -ceq 'wugai-uninstall-file-recovery-v1' -and
        $receipt.owner -ceq $script:WuGaiOwner -and $receipt.install_root -ceq $paths.InstallRoot -and
        $receipt.user_sid -ceq $ExpectedUserSid -and $receipt.maintenance_present -is [bool]) 'Invalid removal recovery receipt.'
    $saved = Assert-WuGaiRemovalBackup $paths.InstallRoot $ExpectedUserSid $receipt.backup $receipt.plan_sha256
    $entry = ConvertTo-WuGaiEntry $receipt.apps_entry
    Assert-WuGaiAppsEntry $paths $entry $ExpectedUserSid
    $maintenanceBackup = Join-Path $saved.Backup 'maintenance'
    if ($receipt.maintenance_present) {
        Assert-WuGai ($receipt.maintenance_setup_sha256 -cmatch '^[0-9a-f]{64}$' -and
            $receipt.maintenance_manifest_sha256 -cmatch '^[0-9a-f]{64}$') 'Invalid maintenance backup identity.'
        $image = Get-WuGaiMaintenanceImage $paths $maintenanceBackup $ExpectedUserSid
        Assert-WuGai ($image.SetupHash -ceq $receipt.maintenance_setup_sha256 -and
            $image.ManifestHash -ceq $receipt.maintenance_manifest_sha256) 'Maintenance backup changed.'
        if ($entry) { Assert-WuGai ($entry.DisplayVersion -ceq $image.Record.version) 'Uninstall entry version changed.' }
        $currentEntry = Read-WuGaiAppsEntry $paths
        Assert-WuGaiEntryTransition $currentEntry $entry $null
        if (Test-Path -LiteralPath $paths.MaintenanceRoot) {
            $currentImage = Get-WuGaiMaintenanceSubset $paths $paths.MaintenanceRoot $ExpectedUserSid
            Assert-WuGai (Test-WuGaiRecordedImage $currentImage $receipt.maintenance_setup_sha256 `
                $receipt.maintenance_manifest_sha256) 'Changed maintenance files block recovery.'
        }
    } else {
        Assert-WuGai ($null -eq $entry -and -not $receipt.maintenance_setup_sha256 -and
            -not $receipt.maintenance_manifest_sha256) 'Unexpected maintenance backup identity.'
        Assert-WuGai (-not (Test-Path -LiteralPath $paths.MaintenanceRoot) -and
            $null -eq (Read-WuGaiAppsEntry $paths)) 'Unexpected maintenance data blocks recovery.'
    }
    # Hold read-only handles so the per-user backup cannot be replaced between
    # verification and elevated restoration. Revalidate after acquiring every
    # handle; a pre-existing writer or a changed file stops recovery.
    $held = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
    try {
        $lockPaths = @((Join-Path $saved.Backup 'plan.json'))
        foreach ($row in $saved.Proof.files) {
            $lockPaths += Join-Path $saved.Backup ('package\'+$row.relative_path.Replace('/','\'))
        }
        if ($receipt.maintenance_present) {
            $lockPaths += (Join-Path $maintenanceBackup 'Uninstall.exe'),(Join-Path $maintenanceBackup 'maintenance.json')
        }
        foreach ($file in $lockPaths) {
            Assert-WuGaiNoReparse $file
            $handle=[IO.File]::Open($file,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
            $held.Add($handle)
        }
        $saved = Assert-WuGaiRemovalBackup $paths.InstallRoot $ExpectedUserSid $receipt.backup $receipt.plan_sha256
        if ($receipt.maintenance_present) {
            $image = Get-WuGaiMaintenanceImage $paths $maintenanceBackup $ExpectedUserSid
            Assert-WuGai ($image.SetupHash -ceq $receipt.maintenance_setup_sha256 -and
                $image.ManifestHash -ceq $receipt.maintenance_manifest_sha256) 'Maintenance backup changed.'
        }
        Assert-WuGai ((Get-WuGaiHash $receiptPath) -ceq $receiptHash) 'Removal recovery receipt changed.'
        $status = Invoke-WuGaiUninstallCli $Cli @('--status','--machine-com-x64')
        Assert-WuGaiAbsentReport $status
        Restore-WuGaiUninstallFiles $paths.InstallRoot $saved.Proof (Join-Path $saved.Backup 'package')
        if ($receipt.maintenance_present) {
            Restore-WuGaiMaintenanceBackup -InstallRoot $paths.InstallRoot -ExpectedUserSid $ExpectedUserSid `
                -BackupRoot $maintenanceBackup -AppsEntry $entry
        }
        Assert-WuGaiUninstallInventory $paths.InstallRoot $saved.Proof
        Remove-WuGaiRemovalReceipt $paths.InstallRoot $receiptHash
    } finally {
        foreach ($handle in $held) { $handle.Dispose() }
    }
}
