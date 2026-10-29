# Shared, side-effect-free definitions for publishing and removing the uninstall entry.
$script:WuGaiOwner = 'LiuKai-TSF-v1'
function Assert-WuGai([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Get-WuGaiHash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-WuGaiCurrentSid { [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
function Assert-WuGaiNoReparse([string]$Path, [switch]$Tree) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            Assert-WuGai (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Reparse point is not supported: $cursor"
        }
        $parent = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\'))
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    if ($Tree -and (Test-Path -LiteralPath $Path)) {
        $pending = New-Object 'System.Collections.Generic.Stack[string]'
        $pending.Push([IO.Path]::GetFullPath($Path))
        while ($pending.Count) {
            foreach ($item in Get-ChildItem -LiteralPath ($pending.Pop()) -Force) {
                Assert-WuGai (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Reparse point is not supported: $($item.FullName)"
                if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            }
        }
    }
}
function Get-WuGaiMaintenancePaths([string]$InstallRoot) {
    Assert-WuGai (-not [string]::IsNullOrWhiteSpace($InstallRoot) -and [IO.Path]::IsPathRooted($InstallRoot)) 'An absolute installation root is required.'
    $root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
    Assert-WuGai ($root -ceq $InstallRoot.TrimEnd('\') -and $root -ne [IO.Path]::GetPathRoot($root).TrimEnd('\') -and
        $root -notmatch '[\x00-\x1f"]') 'Invalid installation root.'
    Assert-WuGaiNoReparse $root
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))).Replace('-','').ToLowerInvariant() }
    finally { $sha.Dispose() }
    [pscustomobject]@{InstallRoot=$root; MaintenanceRoot=($root + '.maintenance');
        Executable=($root + '.maintenance\Uninstall.exe');
        RegistryPath=('Software\Microsoft\Windows\CurrentVersion\Uninstall\WuGai-' + $key)}
}
function Read-WuGaiAppsEntry($Paths) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.OpenSubKey($Paths.RegistryPath, $false)
        if (-not $key) { return $null }
        try {
            Assert-WuGai ($key.SubKeyCount -eq 0 -and $key.ValueCount -le 32) 'Unexpected uninstall registry contents.'
            $entry = @{}
            foreach ($name in $key.GetValueNames()) { $entry[$name] = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
            return $entry
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function Write-WuGaiAppsEntry($Paths, $Entry) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key = $base.CreateSubKey($Paths.RegistryPath)
        try {
            foreach ($name in $Entry.Keys) {
                $kind = if ($Entry[$name] -is [int]) { [Microsoft.Win32.RegistryValueKind]::DWord } else { [Microsoft.Win32.RegistryValueKind]::String }
                $key.SetValue($name, $Entry[$name], $kind)
            }
        } finally { $key.Dispose() }
    } finally { $base.Dispose() }
}
function Remove-WuGaiAppsEntry($Paths) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try { $base.DeleteSubKeyTree($Paths.RegistryPath, $false) } finally { $base.Dispose() }
}
function Assert-WuGaiAppsEntry($Paths, $Entry, [string]$ExpectedUserSid) {
    if ($null -eq $Entry) { return }
    $command = '"' + $Paths.Executable + '" --uninstall --install-root "' + $Paths.InstallRoot + '" --expected-sid ' + $ExpectedUserSid
    Assert-WuGai ($Entry.WuGaiOwner -ceq $script:WuGaiOwner -and $Entry.WuGaiUserSid -ceq $ExpectedUserSid -and
        $Entry.InstallLocation -ceq $Paths.InstallRoot -and $Entry.UninstallString -ceq $command) 'Uninstall entry belongs to another installation or has changed.'
}
function Read-WuGaiMaintenance([string]$InstallRoot, [string]$ExpectedUserSid) {
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    Assert-WuGai ($ExpectedUserSid -match '^S-1-\d+(?:-\d+)+$') 'Invalid expected Windows account.'
    Assert-WuGaiNoReparse $paths.MaintenanceRoot -Tree
    if (-not (Test-Path -LiteralPath $paths.MaintenanceRoot)) { return $null }
    Assert-WuGai (Test-Path -LiteralPath $paths.MaintenanceRoot -PathType Container) 'Maintenance path is not a directory.'
    $children = @(Get-ChildItem -LiteralPath $paths.MaintenanceRoot -Force)
    Assert-WuGai ($children.Count -eq 2 -and @($children | Where-Object { $_.PSIsContainer -or $_.Name -cnotin @('Uninstall.exe','maintenance.json') }).Count -eq 0) 'Unknown maintenance files; retained.'
    $record = Get-Content -LiteralPath (Join-Path $paths.MaintenanceRoot 'maintenance.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-WuGai ($record.format -ceq 'wugai-uninstaller-v1' -and $record.owner -ceq $script:WuGaiOwner -and
        $record.user_sid -ceq $ExpectedUserSid -and $record.install_root -ceq $paths.InstallRoot -and
        $record.maintenance_root -ceq $paths.MaintenanceRoot -and $record.version -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -and
        $record.setup_sha256 -match '^[0-9a-f]{64}$' -and (Get-WuGaiHash $paths.Executable) -ceq $record.setup_sha256) 'Maintenance ownership or hash mismatch.'
    return $record
}
function Remove-WuGaiCreatedTree([string]$Path, [string]$ExpectedParent, [string]$ExpectedLeaf) {
    $absolute = [IO.Path]::GetFullPath($Path)
    Assert-WuGai ((Split-Path -Parent $absolute) -ceq $ExpectedParent -and (Split-Path -Leaf $absolute) -ceq $ExpectedLeaf) 'Unsafe maintenance cleanup target.'
    Assert-WuGaiNoReparse $absolute -Tree
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}
function ConvertTo-WuGaiEntry($Entry) {
    if ($null -eq $Entry) { return $null }
    $result = @{}
    if ($Entry -is [Collections.IDictionary]) { foreach ($name in $Entry.Keys) { $result[$name] = $Entry[$name] } }
    else {
        foreach ($property in $Entry.PSObject.Properties) {
            $value = $property.Value
            # PowerShell 7 deserializes JSON integers as Int64. Publication
            # records were validated as DWORDs before serialization; restore
            # their Int32 representation without accepting QWORDs read from the
            # live registry (which arrive in the IDictionary branch above).
            if ($value -is [long] -and $value -ge [int]::MinValue -and $value -le [int]::MaxValue) { $value = [int]$value }
            $result[$property.Name] = $value
        }
    }
    foreach ($name in $result.Keys) {
        Assert-WuGai ($result[$name] -is [string] -or $result[$name] -is [int]) 'Unsupported uninstall registry value type.'
    }
    return $result
}
function Test-WuGaiEntriesEqual($Left, $Right) {
    $a = ConvertTo-WuGaiEntry $Left; $b = ConvertTo-WuGaiEntry $Right
    if ($null -eq $a -or $null -eq $b) { return $null -eq $a -and $null -eq $b }
    if ($a.Count -ne $b.Count) { return $false }
    foreach ($name in $a.Keys) {
        if (-not $b.ContainsKey($name) -or $a[$name].GetType() -ne $b[$name].GetType() -or $a[$name] -cne $b[$name]) { return $false }
    }
    return $true
}
function Assert-WuGaiEntryTransition($Current, $Before, $After) {
    $currentEntry = ConvertTo-WuGaiEntry $Current
    $beforeEntry = ConvertTo-WuGaiEntry $Before; $afterEntry = ConvertTo-WuGaiEntry $After
    if ($null -eq $currentEntry) { return }
    foreach ($name in $currentEntry.Keys) {
        $known = $false
        foreach ($candidate in @($beforeEntry,$afterEntry)) {
            if ($null -ne $candidate -and $candidate.ContainsKey($name) -and
                $candidate[$name].GetType() -eq $currentEntry[$name].GetType() -and $candidate[$name] -ceq $currentEntry[$name]) { $known = $true }
        }
        Assert-WuGai $known 'Uninstall registry changed outside the recorded transaction; retained.'
    }
}
function Get-WuGaiMaintenanceImage($Paths, [string]$Directory, [string]$ExpectedUserSid) {
    Assert-WuGaiNoReparse $Directory -Tree
    Assert-WuGai (Test-Path -LiteralPath $Directory -PathType Container) 'Maintenance backup is missing.'
    $children = @(Get-ChildItem -LiteralPath $Directory -Force)
    Assert-WuGai ($children.Count -eq 2 -and @($children | Where-Object { $_.PSIsContainer -or $_.Name -cnotin @('Uninstall.exe','maintenance.json') }).Count -eq 0) 'Unknown or incomplete maintenance backup.'
    $manifest = Join-Path $Directory 'maintenance.json'
    $record = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    $setupHash = Get-WuGaiHash (Join-Path $Directory 'Uninstall.exe')
    Assert-WuGai ($record.format -ceq 'wugai-uninstaller-v1' -and $record.owner -ceq $script:WuGaiOwner -and
        $record.user_sid -ceq $ExpectedUserSid -and $record.install_root -ceq $Paths.InstallRoot -and
        $record.maintenance_root -ceq $Paths.MaintenanceRoot -and $record.version -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -and
        -not $record.version.Contains('..') -and $record.setup_sha256 -cmatch '^[0-9a-f]{64}$' -and $setupHash -ceq $record.setup_sha256) 'Maintenance backup ownership or hash mismatch.'
    [pscustomobject]@{Record=$record; SetupHash=$setupHash; ManifestHash=(Get-WuGaiHash $manifest)}
}
function Restore-WuGaiMaintenanceBackup([string]$InstallRoot, [string]$ExpectedUserSid, [string]$BackupRoot, $AppsEntry) {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $ExpectedUserSid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    $backup = [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\')
    Assert-WuGai (-not $backup.Equals($paths.MaintenanceRoot,[StringComparison]::OrdinalIgnoreCase) -and
        -not $backup.StartsWith($paths.MaintenanceRoot+'\',[StringComparison]::OrdinalIgnoreCase) -and
        -not $paths.MaintenanceRoot.StartsWith($backup+'\',[StringComparison]::OrdinalIgnoreCase)) 'Maintenance backup overlaps its restore destination.'
    $image = Get-WuGaiMaintenanceImage $paths $backup $ExpectedUserSid
    $entry = ConvertTo-WuGaiEntry $AppsEntry
    Assert-WuGaiAppsEntry $paths $entry $ExpectedUserSid
    if ($entry) { Assert-WuGai ($entry.DisplayVersion -ceq $image.Record.version) 'Maintenance backup version and uninstall entry differ.' }
    Assert-WuGaiNoReparse $paths.MaintenanceRoot -Tree
    if (Test-Path -LiteralPath $paths.MaintenanceRoot) {
        Assert-WuGai (Test-Path -LiteralPath $paths.MaintenanceRoot -PathType Container) 'Maintenance restore destination is not a directory.'
        foreach ($file in Get-ChildItem -LiteralPath $paths.MaintenanceRoot -Force) {
            Assert-WuGai (-not $file.PSIsContainer -and $file.Name -cin @('Uninstall.exe','maintenance.json') -and
                (Get-WuGaiHash $file.FullName) -ceq (Get-WuGaiHash (Join-Path $backup $file.Name))) 'Changed maintenance files cannot be overwritten during recovery.'
        }
    }
    $currentEntry = Read-WuGaiAppsEntry $paths
    Assert-WuGaiEntryTransition $currentEntry $entry $null
    [IO.Directory]::CreateDirectory($paths.MaintenanceRoot) | Out-Null
    foreach ($name in @('Uninstall.exe','maintenance.json')) {
        $destination = Join-Path $paths.MaintenanceRoot $name
        if (-not (Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath (Join-Path $backup $name) -Destination $destination }
    }
    $restored = Get-WuGaiMaintenanceImage $paths $paths.MaintenanceRoot $ExpectedUserSid
    Assert-WuGai ($restored.SetupHash -ceq $image.SetupHash -and $restored.ManifestHash -ceq $image.ManifestHash) 'Maintenance backup restore verification failed.'
    if ($entry) { Write-WuGaiAppsEntry $paths $entry } elseif ($null -ne $currentEntry) { Remove-WuGaiAppsEntry $paths }
    Assert-WuGai (Test-WuGaiEntriesEqual (Read-WuGaiAppsEntry $paths) $entry) 'Uninstall entry restoration failed.'
}
function Write-WuGaiPublicationReceipt([string]$Path, $Receipt) {
    Assert-WuGaiNoReparse $Path
    $bytes = [Text.Encoding]::UTF8.GetBytes(($Receipt | ConvertTo-Json -Depth 8))
    $stream = New-Object IO.FileStream($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $stream.Write($bytes,0,$bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
}
function Get-WuGaiMaintenanceSubset($Paths, [string]$Directory, [string]$ExpectedUserSid) {
    Assert-WuGaiNoReparse $Directory -Tree
    if (-not (Test-Path -LiteralPath $Directory)) { return $null }
    Assert-WuGai (Test-Path -LiteralPath $Directory -PathType Container) 'Maintenance transaction path is not a directory.'
    $files = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Directory -Force) {
        Assert-WuGai (-not $file.PSIsContainer -and $file.Name -cin @('Uninstall.exe','maintenance.json')) 'Unknown maintenance transaction contents; retained.'
        $files[$file.Name] = Get-WuGaiHash $file.FullName
    }
    $record = if ($files.Count -eq 2) { (Get-WuGaiMaintenanceImage $Paths $Directory $ExpectedUserSid).Record } else { $null }
    [pscustomobject]@{Files=$files; Complete=($files.Count -eq 2); Record=$record}
}
function Test-WuGaiRecordedImage($Image, [string]$SetupHash, [string]$ManifestHash) {
    if ($null -eq $Image) { return $false }
    foreach ($name in $Image.Files.Keys) {
        $expected = if ($name -ceq 'Uninstall.exe') { $SetupHash } else { $ManifestHash }
        if ($Image.Files[$name] -cne $expected) { return $false }
    }
    return $true
}
function Recover-WuGaiMaintenancePublication([string]$InstallRoot, [string]$ExpectedUserSid, [switch]$Rollback) {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $ExpectedUserSid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    $receiptPath = $paths.MaintenanceRoot + '.publish.json'
    if (-not (Test-Path -LiteralPath $receiptPath)) { return }
    Assert-WuGaiNoReparse $receiptPath
    $receiptHash = Get-WuGaiHash $receiptPath
    $receipt = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $parent = Split-Path -Parent $paths.MaintenanceRoot
    $leaf = Split-Path -Leaf $paths.MaintenanceRoot
    Assert-WuGai ($receipt.format -ceq 'wugai-maintenance-publication-v1' -and $receipt.owner -ceq $script:WuGaiOwner -and
        $receipt.user_sid -ceq $ExpectedUserSid -and $receipt.install_root -ceq $paths.InstallRoot -and
        $receipt.maintenance_root -ceq $paths.MaintenanceRoot -and $receipt.old_present -is [bool] -and
        $receipt.stage_leaf -cmatch ('^'+[Regex]::Escape($leaf)+'\.stage-[0-9a-f]{32}$') -and
        $receipt.previous_leaf -cmatch ('^'+[Regex]::Escape($leaf)+'\.previous-[0-9a-f]{32}$') -and
        $receipt.new_setup_sha256 -cmatch '^[0-9a-f]{64}$' -and $receipt.new_manifest_sha256 -cmatch '^[0-9a-f]{64}$') 'Invalid maintenance publication receipt; retained.'
    if ($receipt.old_present) {
        Assert-WuGai ($receipt.old_setup_sha256 -cmatch '^[0-9a-f]{64}$' -and $receipt.old_manifest_sha256 -cmatch '^[0-9a-f]{64}$') 'Invalid previous maintenance identity.'
    } else { Assert-WuGai (-not $receipt.old_setup_sha256 -and -not $receipt.old_manifest_sha256 -and $null -eq $receipt.old_apps) 'Unexpected previous maintenance identity.' }
    $stage = Join-Path $parent $receipt.stage_leaf; $previous = Join-Path $parent $receipt.previous_leaf
    $oldEntry = ConvertTo-WuGaiEntry $receipt.old_apps; $newEntry = ConvertTo-WuGaiEntry $receipt.new_apps
    Assert-WuGaiAppsEntry $paths $oldEntry $ExpectedUserSid
    Assert-WuGai ($null -ne $newEntry) 'Publication receipt has no new uninstall entry.'
    Assert-WuGaiAppsEntry $paths $newEntry $ExpectedUserSid
    $currentEntry = Read-WuGaiAppsEntry $paths
    Assert-WuGaiEntryTransition $currentEntry $oldEntry $newEntry
    $images = @{}
    foreach ($candidate in @($paths.MaintenanceRoot,$stage,$previous)) {
        if (Test-Path -LiteralPath $candidate) { $images[$candidate] = Get-WuGaiMaintenanceSubset $paths $candidate $ExpectedUserSid }
    }
    if ($images.ContainsKey($stage)) {
        Assert-WuGai (Test-WuGaiRecordedImage $images[$stage] $receipt.new_setup_sha256 $receipt.new_manifest_sha256) 'Publication stage changed; retained.'
    }
    if ($images.ContainsKey($previous)) {
        Assert-WuGai ($receipt.old_present -and (Test-WuGaiRecordedImage $images[$previous] $receipt.old_setup_sha256 $receipt.old_manifest_sha256)) 'Previous maintenance changed; retained.'
    }
    $currentIsNew = $false; $currentIsOld = $false
    if ($images.ContainsKey($paths.MaintenanceRoot)) {
        $current = $images[$paths.MaintenanceRoot]
        $currentIsNew = Test-WuGaiRecordedImage $current $receipt.new_setup_sha256 $receipt.new_manifest_sha256
        $currentIsOld = $receipt.old_present -and (Test-WuGaiRecordedImage $current $receipt.old_setup_sha256 $receipt.old_manifest_sha256)
        Assert-WuGai ($currentIsNew -or $currentIsOld) 'Published maintenance changed; retained.'
    }
    $currentOldComplete = $currentIsOld -and $images[$paths.MaintenanceRoot].Complete
    $previousComplete = $images.ContainsKey($previous) -and $images[$previous].Complete
    $complete = -not $Rollback -and $currentIsNew -and $images[$paths.MaintenanceRoot].Complete -and (Test-WuGaiEntriesEqual $currentEntry $newEntry)
    if (-not $complete) {
        Assert-WuGai (-not $receipt.old_present -or $currentOldComplete -or $previousComplete) 'Previous maintenance cannot be proven; retained.'
        if ($images.ContainsKey($paths.MaintenanceRoot) -and -not $currentOldComplete) {
            Assert-WuGai $currentIsNew 'Partially changed previous maintenance cannot be removed.'
            Remove-WuGaiCreatedTree $paths.MaintenanceRoot $parent $leaf
        }
        if ($receipt.old_present -and -not $currentOldComplete) { [IO.Directory]::Move($previous,$paths.MaintenanceRoot); $images.Remove($previous) }
        if ($null -ne $currentEntry) { Remove-WuGaiAppsEntry $paths }
        if ($oldEntry) { Write-WuGaiAppsEntry $paths $oldEntry }
        Assert-WuGai (Test-WuGaiEntriesEqual (Read-WuGaiAppsEntry $paths) $oldEntry) 'Publication registry rollback failed.'
    }
    if ($images.ContainsKey($stage)) { Remove-WuGaiCreatedTree $stage $parent $receipt.stage_leaf }
    if ($images.ContainsKey($previous)) { Remove-WuGaiCreatedTree $previous $parent $receipt.previous_leaf }
    Assert-WuGai ((Get-WuGaiHash $receiptPath) -ceq $receiptHash) 'Publication receipt changed during recovery.'
    Remove-Item -LiteralPath $receiptPath -Force
}
function Publish-WuGaiUninstaller([string]$SetupPath, [string]$InstallRoot, [string]$Version, [string]$ExpectedUserSid) {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $ExpectedUserSid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    Recover-WuGaiMaintenancePublication $paths.InstallRoot $ExpectedUserSid
    Assert-WuGai ($Version -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -and -not $Version.Contains('..')) 'Invalid release version.'
    Assert-WuGai (Test-Path -LiteralPath $SetupPath -PathType Leaf) 'Setup executable is unavailable for the uninstall entry.'
    Assert-WuGaiNoReparse $SetupPath
    Assert-WuGai (Test-Path -LiteralPath $paths.InstallRoot -PathType Container) 'Installation is absent.'
    Assert-WuGai ([IO.File]::ReadAllText((Join-Path $paths.InstallRoot '.liukai-owner')) -ceq "liukai-owner-v1`n$script:WuGaiOwner`n") 'Installation owner mismatch.'
    $state = [IO.File]::ReadAllText((Join-Path $paths.InstallRoot '.liukai-install-state'))
    Assert-WuGai ($state -match ('(?m)^current=' + [Regex]::Escape($Version) + '\r?$')) 'Installed version mismatch.'
    $old = Read-WuGaiMaintenance $paths.InstallRoot $ExpectedUserSid
    $oldEntry = Read-WuGaiAppsEntry $paths
    Assert-WuGaiAppsEntry $paths $oldEntry $ExpectedUserSid
    Assert-WuGai (-not $oldEntry -or $null -ne $old) 'Uninstall entry has no owned maintenance files.'
    $hash = Get-WuGaiHash $SetupPath
    $parent = Split-Path -Parent $paths.MaintenanceRoot
    $stageLeaf = (Split-Path -Leaf $paths.MaintenanceRoot) + '.stage-' + [Guid]::NewGuid().ToString('N')
    $oldLeaf = (Split-Path -Leaf $paths.MaintenanceRoot) + '.previous-' + [Guid]::NewGuid().ToString('N')
    $stage = Join-Path $parent $stageLeaf
    $previous = Join-Path $parent $oldLeaf
    $stageCreated = $false; $commitVerified = $false
    $receiptPath = $paths.MaintenanceRoot + '.publish.json'
    $oldManifestHash = if ($old) { Get-WuGaiHash (Join-Path $paths.MaintenanceRoot 'maintenance.json') } else { '' }
    $command = '"' + $paths.Executable + '" --uninstall --install-root "' + $paths.InstallRoot + '" --expected-sid ' + $ExpectedUserSid
    $entry = [ordered]@{WuGaiOwner=$script:WuGaiOwner; WuGaiUserSid=$ExpectedUserSid; InstallLocation=$paths.InstallRoot;
        DisplayName='WuGai'; DisplayVersion=$Version; UninstallString=$command; QuietUninstallString=($command + ' --silent');
        NoModify=1; NoRepair=1}
    try {
        Assert-WuGai (-not (Test-Path -LiteralPath $stage) -and -not (Test-Path -LiteralPath $previous)) 'Maintenance staging collision.'
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        $stageCreated = $true
        Copy-Item -LiteralPath $SetupPath -Destination (Join-Path $stage 'Uninstall.exe')
        Assert-WuGai ((Get-WuGaiHash (Join-Path $stage 'Uninstall.exe')) -ceq $hash) 'Setup changed while copying.'
        $record = [ordered]@{format='wugai-uninstaller-v1'; owner=$script:WuGaiOwner; user_sid=$ExpectedUserSid;
            install_root=$paths.InstallRoot; maintenance_root=$paths.MaintenanceRoot; version=$Version; setup_sha256=$hash}
        [IO.File]::WriteAllText((Join-Path $stage 'maintenance.json'), ($record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
        $receipt = [ordered]@{format='wugai-maintenance-publication-v1'; owner=$script:WuGaiOwner; user_sid=$ExpectedUserSid;
            install_root=$paths.InstallRoot; maintenance_root=$paths.MaintenanceRoot; stage_leaf=$stageLeaf; previous_leaf=$oldLeaf;
            old_present=($null -ne $old); old_setup_sha256=$(if($old){$old.setup_sha256}else{''}); old_manifest_sha256=$oldManifestHash;
            new_setup_sha256=$hash; new_manifest_sha256=(Get-WuGaiHash (Join-Path $stage 'maintenance.json'));
            old_apps=$oldEntry; new_apps=$entry}
        # A flushed receipt precedes either rename; a later run can classify
        # exact old/new images without trusting an arbitrary cleanup path.
        Write-WuGaiPublicationReceipt $receiptPath $receipt
        if ($old) { [IO.Directory]::Move($paths.MaintenanceRoot, $previous) }
        [IO.Directory]::Move($stage, $paths.MaintenanceRoot)
        Write-WuGaiAppsEntry $paths $entry
        $readback = Read-WuGaiAppsEntry $paths
        Assert-WuGai ($null -ne $readback -and $readback.DisplayVersion -ceq $Version -and
            $readback.QuietUninstallString -ceq $entry.QuietUninstallString) 'Uninstall registry publication readback failed.'
        Assert-WuGaiAppsEntry $paths $readback $ExpectedUserSid
        $verified = Read-WuGaiMaintenance $paths.InstallRoot $ExpectedUserSid
        $commitVerified = $true
        Recover-WuGaiMaintenancePublication $paths.InstallRoot $ExpectedUserSid
        return $verified
    } catch {
        $failure = $_
        if (Test-Path -LiteralPath $receiptPath) {
            try {
                if ($commitVerified) { Recover-WuGaiMaintenancePublication $paths.InstallRoot $ExpectedUserSid }
                else { Recover-WuGaiMaintenancePublication $paths.InstallRoot $ExpectedUserSid -Rollback }
            }
            catch { throw "Maintenance publication failed and recovery evidence was retained at ${receiptPath}: $($_.Exception.Message)" }
        }
        throw $failure
    } finally {
        if ($stageCreated -and -not (Test-Path -LiteralPath $receiptPath) -and (Test-Path -LiteralPath $stage)) {
            Remove-WuGaiCreatedTree $stage $parent $stageLeaf
        }
    }
}
function Remove-WuGaiMaintenance([string]$InstallRoot, [string]$ExpectedUserSid) {
    Assert-WuGai ((Get-WuGaiCurrentSid) -ceq $ExpectedUserSid) 'Windows account changed.'
    $paths = Get-WuGaiMaintenancePaths $InstallRoot
    Recover-WuGaiMaintenancePublication $paths.InstallRoot $ExpectedUserSid
    $record = Read-WuGaiMaintenance $paths.InstallRoot $ExpectedUserSid
    $entry = Read-WuGaiAppsEntry $paths
    Assert-WuGaiAppsEntry $paths $entry $ExpectedUserSid
    Assert-WuGai (-not $entry -or $null -ne $record) 'Uninstall entry has no owned maintenance files.'
    if (-not $record) { return }
    $parent = Split-Path -Parent $paths.MaintenanceRoot
    $leaf = (Split-Path -Leaf $paths.MaintenanceRoot) + '.remove-' + [Guid]::NewGuid().ToString('N')
    $backup = Join-Path $parent $leaf
    Assert-WuGai (-not (Test-Path -LiteralPath $backup)) 'Maintenance removal backup collision.'
    $backupCreated = $false
    try {
        [IO.Directory]::CreateDirectory($backup) | Out-Null
        $backupCreated = $true
        foreach ($name in @('Uninstall.exe','maintenance.json')) {
            Copy-Item -LiteralPath (Join-Path $paths.MaintenanceRoot $name) -Destination (Join-Path $backup $name)
        }
        $before = Get-WuGaiMaintenanceImage $paths $paths.MaintenanceRoot $ExpectedUserSid
        $saved = Get-WuGaiMaintenanceImage $paths $backup $ExpectedUserSid
        Assert-WuGai ($before.SetupHash -ceq $saved.SetupHash -and $before.ManifestHash -ceq $saved.ManifestHash) 'Maintenance removal backup mismatch.'
    } catch {
        $failure = $_
        if ($backupCreated) {
            try {
                Assert-WuGaiNoReparse $backup -Tree
                foreach ($file in Get-ChildItem -LiteralPath $backup -Force) {
                    Assert-WuGai (-not $file.PSIsContainer -and $file.Name -cin @('Uninstall.exe','maintenance.json')) 'Unknown removal backup files must be retained.'
                }
                Remove-WuGaiCreatedTree $backup $parent $leaf
            } catch { throw "Maintenance backup preparation failed; partial backup retained at ${backup}: $($_.Exception.Message)" }
        }
        throw $failure
    }
    try {
        Remove-WuGaiCreatedTree $paths.MaintenanceRoot $parent (Split-Path -Leaf $paths.MaintenanceRoot)
        if ($entry) { Remove-WuGaiAppsEntry $paths }
        Assert-WuGai (-not (Test-Path -LiteralPath $paths.MaintenanceRoot) -and -not (Read-WuGaiAppsEntry $paths)) 'Uninstall entry cleanup incomplete.'
    } catch {
        $failure = $_
        try { Restore-WuGaiMaintenanceBackup $paths.InstallRoot $ExpectedUserSid $backup $entry }
        catch { throw "Maintenance removal failed; verified backup retained at ${backup}: $($_.Exception.Message)" }
        Remove-WuGaiCreatedTree $backup $parent $leaf
        throw $failure
    }
    Remove-WuGaiCreatedTree $backup $parent $leaf
}
