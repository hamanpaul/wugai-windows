$ErrorActionPreference = 'Stop'
$maintenanceTestRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$maintenanceTestSource = Join-Path $maintenanceTestRepo 'native\maintenance.ps1'
$maintenanceTestRoot = Join-Path $maintenanceTestRepo ('build\maintenance-recovery-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $maintenanceTestRoot | Out-Null
$maintenanceTestUtf8 = New-Object Text.UTF8Encoding($false)
$maintenanceTestSid = 'S-1-5-21-111111111-222222222-333333333-1001'
$maintenanceTestOtherSid = 'S-1-5-21-111111111-222222222-333333333-1002'
$script:maintenanceTestPassed = 0
$script:maintenanceTestEntries = @{}
$script:maintenanceTestCurrentSid = $maintenanceTestSid
$script:maintenanceTestWriteFault = $false
$script:maintenanceTestNullAfterWrite = $false
$script:maintenanceTestNullReads = 0
$script:maintenanceTestCopyFault = $false
$script:maintenanceTestWrites = 0
$script:maintenanceTestRemoves = 0
. $maintenanceTestSource

$jsonEntry = '{"NoModify":1}' | ConvertFrom-Json
$normalizedEntry = ConvertTo-WuGaiEntry $jsonEntry
if ($normalizedEntry.NoModify -isnot [int] -or $normalizedEntry.NoModify -ne 1) { throw 'JSON DWORD type was not restored.' }
$rejectedQword = $false
try { ConvertTo-WuGaiEntry @{NoModify=[long]1} | Out-Null } catch { $rejectedQword = $true }
if (-not $rejectedQword) { throw 'A live QWORD was accepted as a DWORD.' }
$rejectedOverflow = $false
try { ConvertTo-WuGaiEntry ('{"NoModify":2147483648}' | ConvertFrom-Json) | Out-Null } catch { $rejectedOverflow = $true }
if (-not $rejectedOverflow) { throw 'An out-of-range JSON integer was accepted.' }

# Reuse only fixture/mocking function definitions. Do not execute the original
# test script or any installation/uninstallation entry point.
$tokens = $null; $errors = $null
$helpers = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'maintenance-tests.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Maintenance test helpers did not parse.' }
foreach ($name in @('Require-MaintenanceTest','Reject-MaintenanceTest','New-MaintenanceFixture',
    'Get-MaintenanceFixtureFiles','Get-WuGaiCurrentSid','Copy-MaintenanceEntry','Read-WuGaiAppsEntry',
    'Write-WuGaiAppsEntry','Remove-WuGaiAppsEntry','Copy-Item','Get-MaintenanceFixtureSnapshot',
    'Reject-UnchangedMaintenance','Publish-MaintenanceFixture','Assert-NoMaintenanceStaging')) {
    $function = $helpers.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name},$false)
    if ($null -eq $function) { throw "Missing fixture helper: $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}
$script:maintenanceTestRemoveTree = (Get-Item Function:\Remove-WuGaiCreatedTree).ScriptBlock
$script:maintenanceTestDeleteFaultPath = $null
function Remove-WuGaiCreatedTree([string]$Path,[string]$ExpectedParent,[string]$ExpectedLeaf) {
    if ($script:maintenanceTestDeleteFaultPath -and $Path -ceq $script:maintenanceTestDeleteFaultPath) {
        $script:maintenanceTestDeleteFaultPath = $null
        # Delete only one known fixture file, then mimic a directory removal
        # failing after it has already made partial progress.
        $absolute = [IO.Path]::GetFullPath($Path)
        if (-not $absolute.StartsWith($maintenanceTestRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test fault target.' }
        Assert-WuGaiNoReparse $absolute -Tree
        [IO.File]::Delete((Join-Path $absolute 'Uninstall.exe'))
        throw 'Injected deletion failure after removing one maintenance file.'
    }
    & $script:maintenanceTestRemoveTree -Path $Path -ExpectedParent $ExpectedParent -ExpectedLeaf $ExpectedLeaf
}

$deletion = New-MaintenanceFixture 'partial maintenance deletion'
Publish-MaintenanceFixture $deletion | Out-Null
$before = Get-MaintenanceFixtureSnapshot $deletion
$script:maintenanceTestDeleteFaultPath = $deletion.Maintenance
Reject-MaintenanceTest { Remove-WuGaiMaintenance $deletion.Install $maintenanceTestSid } 'Partial maintenance deletion did not report failure.'
Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $deletion) -ceq $before) 'Partial maintenance deletion failed to restore the original files and Apps entry.'
Read-WuGaiMaintenance $deletion.Install $maintenanceTestSid | Out-Null
$script:maintenanceTestPassed++
Remove-WuGaiMaintenance $deletion.Install $maintenanceTestSid
Require-MaintenanceTest (-not (Test-Path -LiteralPath $deletion.Maintenance)) 'Removal could not be retried after restored partial deletion.'

$restore = New-MaintenanceFixture 'maintenance backup restore'
Publish-MaintenanceFixture $restore | Out-Null
$restorePaths = Get-WuGaiMaintenancePaths $restore.Install
$savedEntry = Read-WuGaiAppsEntry $restorePaths
$savedSnapshot = Get-MaintenanceFixtureSnapshot $restore
$backup = Join-Path $restore.Directory 'verified maintenance backup'
[IO.Directory]::CreateDirectory($backup) | Out-Null
foreach ($name in @('Uninstall.exe','maintenance.json')) {
    [IO.File]::Copy((Join-Path $restore.Maintenance $name),(Join-Path $backup $name))
}
Remove-WuGaiMaintenance $restore.Install $maintenanceTestSid
Restore-WuGaiMaintenanceBackup -InstallRoot $restore.Install -ExpectedUserSid $maintenanceTestSid -BackupRoot $backup -AppsEntry $savedEntry
Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $restore) -ceq $savedSnapshot) 'Backup restore did not recreate maintenance and its Apps entry.'
[IO.File]::Delete($restorePaths.Executable)
Restore-WuGaiMaintenanceBackup -InstallRoot $restore.Install -ExpectedUserSid $maintenanceTestSid -BackupRoot $backup -AppsEntry $savedEntry
Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $restore) -ceq $savedSnapshot) 'Backup restore did not repair a missing maintenance executable.'
$foreign = Join-Path $restore.Maintenance 'foreign.txt'
[IO.File]::WriteAllText($foreign,'retained unknown file',$maintenanceTestUtf8)
Reject-UnchangedMaintenance $restore { Restore-WuGaiMaintenanceBackup $restore.Install $maintenanceTestSid $backup $savedEntry } 'Maintenance backup restore accepted an unknown file.'
[IO.File]::Delete($foreign)
$backupExecutable = Join-Path $backup 'Uninstall.exe'
$backupBytes = [IO.File]::ReadAllBytes($backupExecutable)
[IO.File]::WriteAllText($backupExecutable,'corrupt backup',$maintenanceTestUtf8)
Reject-UnchangedMaintenance $restore { Restore-WuGaiMaintenanceBackup $restore.Install $maintenanceTestSid $backup $savedEntry } 'Maintenance backup restore accepted a corrupt executable.'
[IO.File]::WriteAllBytes($backupExecutable,$backupBytes)

# The child copies fixture bytes and uses a JSON file as its entire Apps store.
# Environment.Exit deliberately skips catch/finally to model process termination
# between the directory renames and the durable commit decision.
$worker = Join-Path $maintenanceTestRoot 'crash-worker.ps1'
$workerText = @'
param([Parameter(Mandatory=$true)][string]$CasePath)
$ErrorActionPreference = 'Stop'
$script:crashCase = Get-Content -LiteralPath $CasePath -Raw | ConvertFrom-Json
. $script:crashCase.source
function Get-WuGaiCurrentSid { $script:crashCase.sid }
function Read-WuGaiAppsEntry($Paths) {
    if (-not (Test-Path -LiteralPath $script:crashCase.apps)) { return $null }
    $value = Get-Content -LiteralPath $script:crashCase.apps -Raw | ConvertFrom-Json
    $entry = [ordered]@{}
    foreach ($property in $value.PSObject.Properties) { $entry[$property.Name] = $property.Value }
    return $entry
}
function Write-WuGaiAppsEntry($Paths,$Entry) {
    if ($script:crashCase.checkpoint -ceq 'before-apps-write') { [Environment]::Exit(73) }
    [IO.File]::WriteAllText($script:crashCase.apps,($Entry | ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
    [Environment]::Exit(73)
}
function Remove-WuGaiAppsEntry($Paths) { [IO.File]::Delete($script:crashCase.apps) }
Publish-WuGaiUninstaller -SetupPath $script:crashCase.setup -InstallRoot $script:crashCase.install -Version 'r62-test' -ExpectedUserSid $script:crashCase.sid | Out-Null
exit 0
'@
[IO.File]::WriteAllText($worker,$workerText,$maintenanceTestUtf8)
$powershell = (Get-Command powershell.exe).Source
function Start-MaintenanceCrash($Fixture,[string]$Checkpoint) {
    $paths = Get-WuGaiMaintenancePaths $Fixture.Install
    $apps = Join-Path $Fixture.Directory 'fake-apps.json'
    [IO.File]::WriteAllText($apps,((Read-WuGaiAppsEntry $paths) | ConvertTo-Json),$maintenanceTestUtf8)
    $case = [ordered]@{source=$maintenanceTestSource;sid=$maintenanceTestSid;apps=$apps;setup=$Fixture.Setup;install=$Fixture.Install;checkpoint=$Checkpoint}
    $casePath = Join-Path $Fixture.Directory 'crash-case.json'
    [IO.File]::WriteAllText($casePath,($case | ConvertTo-Json),$maintenanceTestUtf8)
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $powershell -NoProfile -ExecutionPolicy Bypass -File $worker -CasePath $casePath 2>&1; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prior }
    Require-MaintenanceTest ($code -eq 73) ("Crash worker missed its checkpoint: " + ($output | Out-String))
    $entry = Get-Content -LiteralPath $apps -Raw | ConvertFrom-Json
    # Recreate registry DWORDs as Int32, rather than leaving PowerShell 7's
    # JSON Int64 values in the fake live-registry dictionary.
    $loaded = ConvertTo-WuGaiEntry $entry
    $script:maintenanceTestEntries[$paths.RegistryPath] = $loaded
    Require-MaintenanceTest (Test-Path -LiteralPath ($Fixture.Install + '.maintenance.publish.json')) 'Crash did not leave a persistent publication receipt.'
}
function Get-PublicationFixtureSnapshot($Fixture) {
    $artifacts = [ordered]@{state=(Get-MaintenanceFixtureSnapshot $Fixture)}
    foreach ($item in @(Get-ChildItem -LiteralPath $Fixture.Directory -Force | Where-Object { $_.Name.StartsWith('Installed WuGai.maintenance') } | Sort-Object Name)) {
        if ($item.PSIsContainer) { $artifacts[$item.Name] = Get-MaintenanceFixtureFiles $item.FullName }
        else { $artifacts[$item.Name] = [Convert]::ToBase64String([IO.File]::ReadAllBytes($item.FullName)) }
    }
    return ConvertTo-Json -InputObject $artifacts -Depth 5 -Compress
}
foreach ($checkpoint in @('before-apps-write','after-apps-write')) {
    $crash = New-MaintenanceFixture $checkpoint
    Publish-MaintenanceFixture $crash | Out-Null
    [IO.File]::WriteAllText($crash.Setup,'new version after interrupted publish',$maintenanceTestUtf8)
    [IO.File]::WriteAllText((Join-Path $crash.Install '.liukai-install-state'),"liukai-install-state-v1`ncurrent=r62-test`n",$maintenanceTestUtf8)
    $beforeCrash = Get-MaintenanceFixtureSnapshot $crash
    Start-MaintenanceCrash $crash $checkpoint
    Recover-WuGaiMaintenancePublication -InstallRoot $crash.Install -ExpectedUserSid $maintenanceTestSid
    Require-MaintenanceTest (-not (Test-Path -LiteralPath ($crash.Install + '.maintenance.publish.json'))) 'Successful recovery left a publication receipt.'
    Assert-NoMaintenanceStaging $crash
    if ($checkpoint -ceq 'before-apps-write') {
        Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $crash) -ceq $beforeCrash) 'Pre-Apps crash did not restore the original maintenance and Apps entry.'
    } else {
        $recovered = Read-WuGaiMaintenance $crash.Install $maintenanceTestSid
        $entry = Read-WuGaiAppsEntry (Get-WuGaiMaintenancePaths $crash.Install)
        Require-MaintenanceTest ($recovered.version -ceq 'r62-test' -and $recovered.setup_sha256 -ceq (Get-WuGaiHash $crash.Setup) -and
            $entry.DisplayVersion -ceq 'r62-test') 'Post-Apps crash did not finish the verified new publication.'
    }
}

$tampered = New-MaintenanceFixture 'receipt validation'
Publish-MaintenanceFixture $tampered | Out-Null
[IO.File]::WriteAllText($tampered.Setup,'new version for receipt validation',$maintenanceTestUtf8)
[IO.File]::WriteAllText((Join-Path $tampered.Install '.liukai-install-state'),"liukai-install-state-v1`ncurrent=r62-test`n",$maintenanceTestUtf8)
Start-MaintenanceCrash $tampered 'before-apps-write'
$receiptPath = $tampered.Install + '.maintenance.publish.json'
$receiptText = [IO.File]::ReadAllText($receiptPath)
foreach ($field in @('user_sid','install_root','maintenance_root','stage_leaf','previous_leaf','new_setup_sha256','old_manifest_sha256')) {
    $receipt = $receiptText | ConvertFrom-Json
    $receipt.$field = switch ($field) {
        'user_sid' { $maintenanceTestOtherSid }
        'stage_leaf' { '..\outside-stage' }
        'previous_leaf' { '..\outside-previous' }
        'new_setup_sha256' { '0' * 64 }
        'old_manifest_sha256' { '0' * 64 }
        default { Join-Path $maintenanceTestRoot 'foreign installation' }
    }
    [IO.File]::WriteAllText($receiptPath,($receipt | ConvertTo-Json -Depth 8),$maintenanceTestUtf8)
    $beforeReceipt = Get-PublicationFixtureSnapshot $tampered
    $writes = $script:maintenanceTestWrites; $removes = $script:maintenanceTestRemoves
    Reject-MaintenanceTest { Recover-WuGaiMaintenancePublication $tampered.Install $maintenanceTestSid } "Recovery accepted changed receipt $field."
    Require-MaintenanceTest ((Get-PublicationFixtureSnapshot $tampered) -ceq $beforeReceipt -and
        $writes -eq $script:maintenanceTestWrites -and $removes -eq $script:maintenanceTestRemoves) "Rejected receipt $field changed files or Apps."
}
[IO.File]::WriteAllText($receiptPath,$receiptText,$maintenanceTestUtf8)
Recover-WuGaiMaintenancePublication $tampered.Install $maintenanceTestSid
Require-MaintenanceTest (-not (Test-Path -LiteralPath $receiptPath)) 'Valid receipt could not recover after rejected tampering.'

# Recovery itself may be interrupted while removing the newly published tree
# (rollback) or the previous tree (committed cleanup). A second recovery must
# use the same receipt and only the still-matching subset of those files.
foreach ($checkpoint in @('before-apps-write','after-apps-write')) {
    $retry = New-MaintenanceFixture ('recovery deletion retry ' + $checkpoint)
    Publish-MaintenanceFixture $retry | Out-Null
    [IO.File]::WriteAllText($retry.Setup,'new version for cleanup retry',$maintenanceTestUtf8)
    [IO.File]::WriteAllText((Join-Path $retry.Install '.liukai-install-state'),"liukai-install-state-v1`ncurrent=r62-test`n",$maintenanceTestUtf8)
    $beforeRetry = Get-MaintenanceFixtureSnapshot $retry
    Start-MaintenanceCrash $retry $checkpoint
    $retryReceipt = $retry.Install + '.maintenance.publish.json'
    $receipt = Get-Content -LiteralPath $retryReceipt -Raw | ConvertFrom-Json
    $script:maintenanceTestDeleteFaultPath = if ($checkpoint -ceq 'before-apps-write') { $retry.Maintenance }
        else { Join-Path $retry.Directory $receipt.previous_leaf }
    Reject-MaintenanceTest { Recover-WuGaiMaintenancePublication $retry.Install $maintenanceTestSid } 'Injected recovery deletion failure was ignored.'
    Require-MaintenanceTest (Test-Path -LiteralPath $retryReceipt) 'Failed recovery deleted its durable receipt.'
    Recover-WuGaiMaintenancePublication $retry.Install $maintenanceTestSid
    Require-MaintenanceTest (-not (Test-Path -LiteralPath $retryReceipt)) 'Retried recovery did not finish.'
    Assert-NoMaintenanceStaging $retry
    if ($checkpoint -ceq 'before-apps-write') {
        Require-MaintenanceTest ((Get-MaintenanceFixtureSnapshot $retry) -ceq $beforeRetry) 'Retried rollback did not preserve the original publication.'
    } else {
        $recovered = Read-WuGaiMaintenance $retry.Install $maintenanceTestSid
        Require-MaintenanceTest ($recovered.version -ceq 'r62-test' -and $recovered.setup_sha256 -ceq (Get-WuGaiHash $retry.Setup)) 'Retried committed cleanup reverted the verified new publication.'
    }
}
Write-Output "maintenance_recovery_tests_passed=$script:maintenanceTestPassed; registry=fake; installer_invocations=0"
