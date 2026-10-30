$ErrorActionPreference = 'Stop'
$maintenanceTestRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$maintenanceTestSource = Join-Path $maintenanceTestRepo 'native\maintenance.ps1'
$maintenanceTestRoot = Join-Path $maintenanceTestRepo ('build\maintenance-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $maintenanceTestRoot | Out-Null
$maintenanceTestUtf8 = New-Object Text.UTF8Encoding($false)
$maintenanceTestSid = 'S-1-5-21-111111111-222222222-333333333-1001'
$maintenanceTestOtherSid = 'S-1-5-21-111111111-222222222-333333333-1002'
$script:maintenanceTestPassed = 0

function Require-MaintenanceTest([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:maintenanceTestPassed++
}
function Reject-MaintenanceTest([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require-MaintenanceTest $rejected $Message
}
function New-MaintenanceFixture([string]$Name) {
    $directory = Join-Path $maintenanceTestRoot $Name
    $install = Join-Path $directory 'Installed WuGai'
    New-Item -ItemType Directory -Path $install -Force | Out-Null
    $setup = Join-Path $directory 'WuGai Setup.exe'
    # The publisher only copies these fixture bytes. They are never executed.
    [IO.File]::WriteAllText($setup, 'setup-fixture-one', $maintenanceTestUtf8)
    [IO.File]::WriteAllText((Join-Path $install 'keep.txt'), 'installed-payload-sentinel', $maintenanceTestUtf8)
    [IO.File]::WriteAllText((Join-Path $install '.liukai-owner'), "liukai-owner-v1`nLiuKai-TSF-v1`n", $maintenanceTestUtf8)
    [IO.File]::WriteAllText((Join-Path $install '.liukai-install-state'), "liukai-install-state-v1`ncurrent=r61-test`n", $maintenanceTestUtf8)
    return [pscustomobject]@{ Directory=$directory; Install=$install; Setup=$setup; Maintenance=($install + '.maintenance') }
}
function Get-MaintenanceFixtureFiles([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '<absent>' }
    $item = Get-Item -LiteralPath $Path -Force
    $rows = @([ordered]@{name='.';attributes=[int]$item.Attributes})
    # Never descend through a reparse point, including one deliberately created
    # by a rejection case. Every fixture and junction target stays in build/.
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0 -and $item.PSIsContainer) {
        foreach ($child in @(Get-ChildItem -LiteralPath $Path -Force | Sort-Object Name)) {
            $row = [ordered]@{name=$child.Name;attributes=[int]$child.Attributes}
            if (-not $child.PSIsContainer -and ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                $row.bytes = [Convert]::ToBase64String([IO.File]::ReadAllBytes($child.FullName))
            }
            $rows += $row
        }
    }
    return ConvertTo-Json -InputObject $rows -Depth 5 -Compress
}

. $maintenanceTestSource

# Replace every Apps/identity boundary before invoking any production function.
# These functions never open HKCU or HKLM. Each fake key is indexed by the real
# production path so multiple roots cannot accidentally share a test entry.
$script:maintenanceTestEntries = @{}
$script:maintenanceTestCurrentSid = $maintenanceTestSid
$script:maintenanceTestWriteFault = $false
$script:maintenanceTestNullAfterWrite = $false
$script:maintenanceTestNullReads = 0
$script:maintenanceTestCopyFault = $false
$script:maintenanceTestWrites = 0
$script:maintenanceTestRemoves = 0
function Get-WuGaiCurrentSid { $script:maintenanceTestCurrentSid }
function Copy-MaintenanceEntry($Entry) {
    if ($null -eq $Entry) { return $null }
    $copy = [ordered]@{}
    foreach ($key in $Entry.Keys) { $copy[$key] = $Entry[$key] }
    return $copy
}
function Read-WuGaiAppsEntry($Paths) {
    if ($script:maintenanceTestNullReads -gt 0) {
        $script:maintenanceTestNullReads--
        return $null
    }
    return Copy-MaintenanceEntry $script:maintenanceTestEntries[$Paths.RegistryPath]
}
function Write-WuGaiAppsEntry($Paths, $Entry) {
    $script:maintenanceTestWrites++
    $script:maintenanceTestEntries[$Paths.RegistryPath] = Copy-MaintenanceEntry $Entry
    if ($script:maintenanceTestNullAfterWrite) {
        $script:maintenanceTestNullAfterWrite = $false
        $script:maintenanceTestNullReads = 1
    }
    if ($script:maintenanceTestWriteFault) {
        $script:maintenanceTestWriteFault = $false
        throw 'Injected Apps publication failure after its write.'
    }
}
function Remove-WuGaiAppsEntry($Paths) {
    $script:maintenanceTestRemoves++
    $script:maintenanceTestEntries.Remove($Paths.RegistryPath)
}
function Copy-Item([string]$LiteralPath, [string]$Destination) {
    if ($script:maintenanceTestCopyFault) {
        $script:maintenanceTestCopyFault = $false
        throw 'Injected setup copy failure.'
    }
    Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination
}
function Get-MaintenanceFixtureSnapshot($Fixture) {
    $paths = Get-WuGaiMaintenancePaths $Fixture.Install
    $entry = $script:maintenanceTestEntries[$paths.RegistryPath]
    # Registry value ordering is not part of its identity. Recovery may rebuild
    # an equivalent hashtable after parsing a durable JSON receipt.
    $canonicalEntry = $null
    if ($null -ne $entry) {
        $canonicalEntry = [ordered]@{}
        foreach ($name in @($entry.Keys | Sort-Object)) { $canonicalEntry[$name] = $entry[$name] }
    }
    $state = [ordered]@{
        maintenance=(Get-MaintenanceFixtureFiles $Fixture.Maintenance)
        installed=(Get-MaintenanceFixtureFiles $Fixture.Install)
        apps=$canonicalEntry
    }
    return ConvertTo-Json -InputObject $state -Depth 6 -Compress
}
function Reject-UnchangedMaintenance($Fixture, [scriptblock]$Action, [string]$Message) {
    $before = Get-MaintenanceFixtureSnapshot $Fixture
    $writes = $script:maintenanceTestWrites
    $removes = $script:maintenanceTestRemoves
    Reject-MaintenanceTest $Action $Message
    Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $Fixture) -ceq $before) "$Message (changed fixture state)"
    Require-MaintenanceTest ($writes -eq $script:maintenanceTestWrites -and $removes -eq $script:maintenanceTestRemoves) "$Message (changed Apps entry)"
}
function Publish-MaintenanceFixture($Fixture, [string]$Version = 'r61-test') {
    Publish-WuGaiUninstaller -SetupPath $Fixture.Setup -InstallRoot $Fixture.Install -Version $Version -ExpectedUserSid $maintenanceTestSid
}
function Assert-NoMaintenanceStaging($Fixture) {
    $left = @(Get-ChildItem -LiteralPath $Fixture.Directory -Force | Where-Object { $_.Name -match '\.maintenance\.(stage|previous)-' })
    Require-MaintenanceTest ($left.Count -eq 0) 'Temporary or prior maintenance directory was left behind.'
}

$fresh = New-MaintenanceFixture 'fresh and upgrade'
$paths = Get-WuGaiMaintenancePaths -InstallRoot $fresh.Install
Require-MaintenanceTest ($paths.InstallRoot -ceq $fresh.Install -and $paths.MaintenanceRoot -ceq $fresh.Maintenance -and
    $paths.Executable -ceq (Join-Path $fresh.Maintenance 'Uninstall.exe')) 'Maintenance paths do not bind to the installation root.'
Require-MaintenanceTest ($paths.RegistryPath -match '^Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WuGai-[0-9a-f]{64}$') 'Apps key is not scoped to the installation root.'
Require-MaintenanceTest ($null -eq (Read-WuGaiMaintenance -InstallRoot $fresh.Install -ExpectedUserSid $maintenanceTestSid)) 'Absent maintenance was not reported as absent.'
foreach ($badPath in @('', 'relative-root', [IO.Path]::GetPathRoot($fresh.Install), ($fresh.Install + '\..\other'), ($fresh.Install + '"bad'))) {
    Reject-MaintenanceTest { Get-WuGaiMaintenancePaths -InstallRoot $badPath } 'An unsafe installation path was accepted.'
}
$record = Publish-MaintenanceFixture $fresh
Require-MaintenanceTest ($record.format -ceq 'wugai-uninstaller-v1' -and $record.user_sid -ceq $maintenanceTestSid -and
    $record.version -ceq 'r61-test' -and $record.install_root -ceq $fresh.Install -and
    $record.setup_sha256 -ceq (Get-WuGaiHash $fresh.Setup)) 'Published maintenance identity or hash was wrong.'
Require-MaintenanceTest (@(Get-ChildItem -LiteralPath $fresh.Maintenance -Force).Count -eq 2) 'Publication did not create exactly two maintenance files.'
$entry = Read-WuGaiAppsEntry $paths
$command = '"' + $paths.Executable + '" --uninstall --install-root "' + $fresh.Install + '" --expected-sid ' + $maintenanceTestSid
Require-MaintenanceTest ($entry.UninstallString -ceq $command -and $entry.QuietUninstallString -ceq ($command + ' --silent')) 'Uninstall command quoting or arguments were wrong.'
Require-MaintenanceTest ($entry.WuGaiOwner -ceq 'LiuKai-TSF-v1' -and $entry.WuGaiUserSid -ceq $maintenanceTestSid -and
    $entry.InstallLocation -ceq $fresh.Install -and $entry.DisplayName -ceq 'WuGai' -and $entry.DisplayVersion -ceq 'r61-test' -and
    $entry.NoModify -is [int] -and $entry.NoModify -eq 1 -and $entry.NoRepair -is [int] -and $entry.NoRepair -eq 1) 'Apps entry fields were wrong.'
Assert-NoMaintenanceStaging $fresh
[IO.File]::WriteAllText($fresh.Setup, 'setup-fixture-two', $maintenanceTestUtf8)
[IO.File]::WriteAllText((Join-Path $fresh.Install '.liukai-install-state'), "liukai-install-state-v1`ncurrent=r62-test`n", $maintenanceTestUtf8)
$upgraded = Publish-MaintenanceFixture $fresh 'r62-test'
Require-MaintenanceTest ($upgraded.version -ceq 'r62-test' -and $upgraded.setup_sha256 -ceq (Get-WuGaiHash $fresh.Setup) -and
    (Read-WuGaiAppsEntry $paths).DisplayVersion -ceq 'r62-test') 'Upgrade did not replace maintenance and its Apps version.'
Assert-NoMaintenanceStaging $fresh
Remove-WuGaiMaintenance -InstallRoot $fresh.Install -ExpectedUserSid $maintenanceTestSid
Require-MaintenanceTest (-not (Test-Path -LiteralPath $fresh.Maintenance) -and $null -eq (Read-WuGaiAppsEntry $paths)) 'Successful maintenance removal left owned state.'
Require-MaintenanceTest ([IO.File]::ReadAllText((Join-Path $fresh.Install 'keep.txt')) -ceq 'installed-payload-sentinel') 'Maintenance removal touched installed payload.'

$guarded = New-MaintenanceFixture 'ownership guards'
Publish-MaintenanceFixture $guarded | Out-Null
$guardPaths = Get-WuGaiMaintenancePaths $guarded.Install
$manifestPath = Join-Path $guarded.Maintenance 'maintenance.json'
$originalManifest = [IO.File]::ReadAllText($manifestPath)
[IO.File]::WriteAllText((Join-Path $guarded.Maintenance 'foreign.txt'), 'unknown user file', $maintenanceTestUtf8)
Reject-UnchangedMaintenance $guarded { Read-WuGaiMaintenance $guarded.Install $maintenanceTestSid } 'Read accepted unknown maintenance files.'
Reject-UnchangedMaintenance $guarded { Publish-MaintenanceFixture $guarded } 'Publish accepted unknown maintenance files.'
Reject-UnchangedMaintenance $guarded { Remove-WuGaiMaintenance $guarded.Install $maintenanceTestSid } 'Remove accepted unknown maintenance files.'
[IO.File]::Delete((Join-Path $guarded.Maintenance 'foreign.txt'))
$script:maintenanceTestCurrentSid = $maintenanceTestOtherSid
Reject-UnchangedMaintenance $guarded { Read-WuGaiMaintenance $guarded.Install $maintenanceTestOtherSid } 'Read accepted another SID.'
Reject-UnchangedMaintenance $guarded { Publish-WuGaiUninstaller $guarded.Setup $guarded.Install 'r61-test' $maintenanceTestOtherSid } 'Publish accepted another SID.'
Reject-UnchangedMaintenance $guarded { Remove-WuGaiMaintenance $guarded.Install $maintenanceTestOtherSid } 'Remove accepted another SID.'
$script:maintenanceTestCurrentSid = $maintenanceTestSid
foreach ($field in @('install_root','maintenance_root','owner','setup_sha256')) {
    $changed = $originalManifest | ConvertFrom-Json
    $changed.$field = if ($field -eq 'setup_sha256') { '0' * 64 } else { 'foreign-value' }
    [IO.File]::WriteAllText($manifestPath, ($changed | ConvertTo-Json), $maintenanceTestUtf8)
    Reject-UnchangedMaintenance $guarded { Read-WuGaiMaintenance $guarded.Install $maintenanceTestSid } "Read accepted altered $field."
    Reject-UnchangedMaintenance $guarded { Publish-MaintenanceFixture $guarded } "Publish accepted altered $field."
    Reject-UnchangedMaintenance $guarded { Remove-WuGaiMaintenance $guarded.Install $maintenanceTestSid } "Remove accepted altered $field."
}
[IO.File]::WriteAllText($manifestPath, $originalManifest, $maintenanceTestUtf8)
$originalExe = [IO.File]::ReadAllBytes($guardPaths.Executable)
[IO.File]::WriteAllText($guardPaths.Executable, 'drifted uninstaller', $maintenanceTestUtf8)
Reject-UnchangedMaintenance $guarded { Read-WuGaiMaintenance $guarded.Install $maintenanceTestSid } 'Read accepted executable drift.'
Reject-UnchangedMaintenance $guarded { Publish-MaintenanceFixture $guarded } 'Publish accepted executable drift.'
Reject-UnchangedMaintenance $guarded { Remove-WuGaiMaintenance $guarded.Install $maintenanceTestSid } 'Remove accepted executable drift.'
[IO.File]::WriteAllBytes($guardPaths.Executable, $originalExe)
$savedEntry = Copy-MaintenanceEntry $script:maintenanceTestEntries[$guardPaths.RegistryPath]
$script:maintenanceTestEntries[$guardPaths.RegistryPath].InstallLocation = (Join-Path $guarded.Directory 'Other installation')
Reject-UnchangedMaintenance $guarded { Publish-MaintenanceFixture $guarded } 'Publish adopted another Apps installation.'
Reject-UnchangedMaintenance $guarded { Remove-WuGaiMaintenance $guarded.Install $maintenanceTestSid } 'Remove adopted another Apps installation.'
$script:maintenanceTestEntries[$guardPaths.RegistryPath] = $savedEntry

$junction = New-MaintenanceFixture 'reparse guard'
$junctionTarget = Join-Path $junction.Directory 'junction target'
New-Item -ItemType Directory -Path $junctionTarget | Out-Null
[IO.File]::WriteAllText((Join-Path $junctionTarget 'keep.txt'), 'junction-target-sentinel', $maintenanceTestUtf8)
New-Item -ItemType Junction -Path $junction.Maintenance -Target $junctionTarget | Out-Null
Reject-UnchangedMaintenance $junction { Read-WuGaiMaintenance $junction.Install $maintenanceTestSid } 'Read followed a maintenance junction.'
Reject-UnchangedMaintenance $junction { Publish-MaintenanceFixture $junction } 'Publish followed a maintenance junction.'
Reject-UnchangedMaintenance $junction { Remove-WuGaiMaintenance $junction.Install $maintenanceTestSid } 'Remove followed a maintenance junction.'
Require-MaintenanceTest ([IO.File]::ReadAllText((Join-Path $junctionTarget 'keep.txt')) -ceq 'junction-target-sentinel') 'A rejected junction operation touched its target.'

foreach ($fault in @('copy','write','null-readback')) {
    foreach ($upgrade in @($false,$true)) {
        $failureFixture = New-MaintenanceFixture ($fault + '-' + $upgrade)
        if ($upgrade) {
            Publish-MaintenanceFixture $failureFixture | Out-Null
            [IO.File]::WriteAllText($failureFixture.Setup, 'new setup for failed upgrade', $maintenanceTestUtf8)
        }
        $before = Get-MaintenanceFixtureSnapshot $failureFixture
        switch ($fault) {
            'copy' { $script:maintenanceTestCopyFault = $true }
            'write' { $script:maintenanceTestWriteFault = $true }
            'null-readback' { $script:maintenanceTestNullAfterWrite = $true }
        }
        Reject-MaintenanceTest { Publish-MaintenanceFixture $failureFixture } "Publication accepted injected $fault (upgrade=$upgrade)."
        Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $failureFixture) -ceq $before) "Failed $fault publication did not restore the prior files and Apps entry (upgrade=$upgrade)."
        Assert-NoMaintenanceStaging $failureFixture
    }
}
Write-Output "maintenance_tests_passed=$script:maintenanceTestPassed; registry=fake; executable_invocations=0"
