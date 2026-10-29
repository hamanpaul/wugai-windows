$ErrorActionPreference='Stop'
$repo=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$testRoot=Join-Path $repo ('build\uninstall-file-recovery-tests-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
. (Join-Path $repo 'native\maintenance.ps1')
. (Join-Path $repo 'native\uninstall-file-recovery.ps1')
$testSid='S-1-5-21-111111111-222222222-333333333-1001'
$script:currentSid=$testSid
$script:entries=@{}
$script:statusAbsent=$true
$script:probeBackupMove=$null
$script:backupMoveBlocked=$false
$script:passed=0
function Require-Test([bool]$Value,[string]$Message) { if(-not $Value){throw $Message};$script:passed++ }
function Reject-Test([scriptblock]$Action,[string]$Message) {
    $rejected=$false;try {& $Action | Out-Null} catch {$rejected=$true}
    Require-Test $rejected $Message
}
function Get-WuGaiCurrentSid { return $script:currentSid }
function Read-WuGaiAppsEntry($Paths) { return $script:entries[$Paths.RegistryPath] }
function Write-WuGaiAppsEntry($Paths,$Entry) { $script:entries[$Paths.RegistryPath]=$Entry }
function Remove-WuGaiAppsEntry($Paths) { $script:entries.Remove($Paths.RegistryPath) }
function Get-WuGaiRemovalReceiptPath([string]$InstallRoot) {
    $key=Split-Path -Leaf (Get-WuGaiMaintenancePaths $InstallRoot).RegistryPath
    return (Join-Path (Join-Path $testRoot 'receipts') ($key+'.json'))
}
function Invoke-WuGaiUninstallCli([string]$Cli,[string[]]$Arguments,[string]$LogPath='') {
    if ($script:probeBackupMove) {
        $source=[IO.Path]::GetFullPath($script:probeBackupMove)
        $destination=[IO.Path]::GetFullPath($script:probeBackupMove+'-move-probe')
        if (-not $source.StartsWith($testRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or
            -not $destination.StartsWith($testRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe backup-move fixture.' }
        try { [IO.Directory]::Move($source,$destination) }
        catch { $script:backupMoveBlocked=$true }
        $script:probeBackupMove=$null
    }
    $names=@('com_x64','com_x86','profile','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')
    $resources=@($names | ForEach-Object {[pscustomobject]@{name=$_;read_result=0;present=$false;lifecycle_owned=$false}})
    return [pscustomobject]@{kind='windows-installer-lifecycle';operation=0;cleanup=0;readback=0;
        profile_present_result=0;profile_present=(-not $script:statusAbsent);profile_registry_absence_result=0;
        all_categories_result=0;all_categories_count=0;resources=$resources}
}
$oldLocalAppData=$env:LOCALAPPDATA
$env:LOCALAPPDATA=Join-Path $testRoot 'local-app-data'
try {
    $root=Join-Path $testRoot 'Installed WuGai'
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $setup=Join-Path $testRoot 'Setup.exe'
    [IO.File]::WriteAllText($setup,'uninstaller fixture')
    foreach($relative in @('.liukai-owner','LiuKaiInstaller.exe','shared/branding.dll','versions/r61/x64/BoshiamyTsf.dll')) {
        $file=Join-Path $root $relative.Replace('/','\')
        [IO.Directory]::CreateDirectory((Split-Path -Parent $file))|Out-Null
        [IO.File]::WriteAllText($file,('fixture:'+ $relative))
    }
    [IO.File]::WriteAllText((Join-Path $root '.liukai-owner'),"liukai-owner-v1`nLiuKai-TSF-v1`n")
    [IO.File]::WriteAllText((Join-Path $root '.liukai-install-state'),"liukai-install-state-v1`ncurrent=r61`n")
    $maintenance=Publish-WuGaiUninstaller -SetupPath $setup -InstallRoot $root -Version 'r61' -ExpectedUserSid $testSid
    $paths=Get-WuGaiMaintenancePaths $root
    $entry=Read-WuGaiAppsEntry $paths
    $backup=Join-Path (Join-Path $env:LOCALAPPDATA 'LiuKai\uninstall-backups') ('20261028T090000Z-'+[Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($backup)|Out-Null
    $rows=@()
    foreach($file in Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName) {
        $relative=$file.FullName.Substring($root.Length+1).Replace('\','/')
        $destination=Join-Path $backup ('package\'+$relative.Replace('/','\'))
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination))|Out-Null
        [IO.File]::Copy($file.FullName,$destination)
        $rows += [pscustomobject]@{relative_path=$relative;sha256=(Get-WuGaiHash $file.FullName);bytes=$file.Length}
    }
    [string[]]$names=@($rows | ForEach-Object {$_.relative_path});[Array]::Sort($names,[StringComparer]::Ordinal)
    $byName=@{};foreach($row in $rows){$byName[$row.relative_path]=$row}
    $material="liukai-uninstall-inventory-v1`n";foreach($name in $names){$material+=$name+"`t"+$byName[$name].sha256+"`n"}
    $hash=[Security.Cryptography.SHA256]::Create()
    try {$fingerprint=[BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($material))).Replace('-','').ToLowerInvariant()} finally {$hash.Dispose()}
    $proof=[pscustomobject]@{schema_version=1;executed=$false;install_root=$root;user_sid=$testSid;
        inventory_fingerprint=$fingerprint;manifest_sha256=('a'*64);files=$rows;registrations_absent=$false}
    $plan=[pscustomobject]@{kind='windows-installer-lifecycle';operation=0;cleanup=0;readback=0;package_uninstall=$proof}
    [IO.File]::WriteAllText((Join-Path $backup 'plan.json'),($plan | ConvertTo-Json -Depth 20),(New-Object Text.UTF8Encoding($false)))
    $maintenanceBackup=Join-Path $backup 'maintenance';[IO.Directory]::CreateDirectory($maintenanceBackup)|Out-Null
    foreach($name in @('Uninstall.exe','maintenance.json')) {[IO.File]::Copy((Join-Path $paths.MaintenanceRoot $name),(Join-Path $maintenanceBackup $name))}
    $expectedPlanHash=Get-WuGaiHash (Join-Path $backup 'plan.json')
    $expectedMaintenanceHash=Get-WuGaiHash (Join-Path $maintenanceBackup 'maintenance.json')
    Reject-Test {Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash ('0'*64) -ExpectedMaintenanceManifestHash $expectedMaintenanceHash} 'An altered plan identity was accepted.'
    Reject-Test {Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash ('0'*64)} 'An altered maintenance identity was accepted.'
    Require-Test (-not(Test-Path -LiteralPath (Get-WuGaiRemovalReceiptPath $root))) 'A rejected preflight created a recovery receipt.'
    $originalWriter=(Get-Item Function:\Write-WuGaiPublicationReceipt).ScriptBlock
    Set-Item Function:\Write-WuGaiPublicationReceipt -Value {
        param([string]$Path,$Receipt)
        [IO.File]::WriteAllText($Path,'partial fixture')
        throw 'Injected receipt write failure.'
    }
    try {
        Reject-Test {Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash $expectedMaintenanceHash} 'A failed receipt write was accepted.'
        Require-Test (-not(Test-Path -LiteralPath (Get-WuGaiRemovalReceiptPath $root))) 'A partial write appeared as a final receipt.'
    } finally { Set-Item Function:\Write-WuGaiPublicationReceipt -Value $originalWriter }

    $receipt=Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash $expectedMaintenanceHash
    Require-Test (Test-Path -LiteralPath $receipt.Path) 'Recovery receipt was not persisted.'
    Reject-Test {Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash $expectedMaintenanceHash} 'An existing recovery receipt was overwritten.'
    Require-Test ((Get-WuGaiHash $receipt.Path) -ceq $receipt.Hash) 'An existing recovery receipt changed during rejected publication.'
    Require-Test (@(Get-ChildItem -LiteralPath (Split-Path -Parent $receipt.Path) -Filter '*.pending-*' -File).Count -eq 0) 'A failed receipt publication left a temporary file.'
    $missing=Join-Path $root 'shared\branding.dll'
    $ready=Join-Path $testRoot 'crash-ready.txt'
    $childScript=Join-Path $PSScriptRoot 'uninstall-file-recovery-crash-fixture.ps1'
    $powershell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $child=Start-Process -FilePath $powershell -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive',
        '-ExecutionPolicy','Bypass','-File',('"'+$childScript+'"'),'-FixtureRoot',('"'+$root+'"'),'-ReadyFile',('"'+$ready+'"')) `
        -WindowStyle Hidden -PassThru
    try {
        $deadline=[DateTime]::UtcNow.AddSeconds(12)
        while (-not (Test-Path -LiteralPath $ready) -and [DateTime]::UtcNow -lt $deadline) {
            if($child.HasExited){throw 'Crash fixture exited before deleting the test files.'}
            Start-Sleep -Milliseconds 100
        }
        Require-Test (Test-Path -LiteralPath $ready) 'Crash fixture did not reach the deletion checkpoint.'
        $child.Kill();$child.WaitForExit(5000)|Out-Null
        Require-Test ($child.HasExited -and -not(Test-Path -LiteralPath $missing) -and -not(Test-Path -LiteralPath $paths.Executable)) 'Crash fixture did not leave the expected partial deletion.'
    } finally {
        if(-not $child.HasExited){$child.Kill();$child.WaitForExit(5000)|Out-Null}
        $child.Dispose()
    }
    $script:entries.Remove($paths.RegistryPath)
    # The recovery implementation has opened the backup files before its
    # status read. A concurrent directory replacement must not be allowed.
    $script:probeBackupMove=$backup
    Recover-WuGaiInterruptedFileRemoval -InstallRoot $root -ExpectedUserSid $testSid -Cli 'fixture-only'
    Require-Test $script:backupMoveBlocked 'An opened backup directory could be renamed during recovery.'
    Require-Test ((Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $paths.Executable)) 'Interrupted file removal was not restored.'
    Require-Test ((Test-WuGaiEntriesEqual (Read-WuGaiAppsEntry $paths) $entry) -and -not(Test-Path -LiteralPath $receipt.Path)) 'Apps entry was not restored or receipt remained.'
    Assert-WuGaiUninstallInventory $root $proof

    $receipt=Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash $expectedMaintenanceHash
    [IO.File]::Delete($missing)
    $backupFile=Join-Path $backup 'package\shared\branding.dll';$original=[IO.File]::ReadAllBytes($backupFile)
    $planPath=Join-Path $backup 'plan.json';$planBytes=[IO.File]::ReadAllBytes($planPath)
    [IO.File]::WriteAllText($planPath,'changed plan')
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'A changed backup plan was accepted.'
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'Changed plan caused writes or lost evidence.'
    [IO.File]::WriteAllBytes($planPath,$planBytes)
    $maintenanceFile=Join-Path $maintenanceBackup 'Uninstall.exe';$maintenanceBytes=[IO.File]::ReadAllBytes($maintenanceFile)
    [IO.File]::WriteAllText($maintenanceFile,'changed uninstaller backup')
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'A changed maintenance backup was accepted.'
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'Changed maintenance backup caused writes or lost evidence.'
    [IO.File]::WriteAllBytes($maintenanceFile,$maintenanceBytes)
    $writer=[IO.File]::Open($backupFile,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
    try { Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'An active backup writer was accepted.' }
    finally {$writer.Dispose()}
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'A locked backup caused writes or lost evidence.'
    [IO.File]::WriteAllText($backupFile,'altered backup')
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'Changed backup was accepted.'
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'Changed backup caused writes or lost evidence.'
    [IO.File]::WriteAllBytes($backupFile,$original)
    $script:currentSid='S-1-5-21-111111111-222222222-333333333-1002'
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'Wrong user was accepted.'
    $script:currentSid=$testSid
    $script:statusAbsent=$false
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'A present profile was accepted for file recovery.'
    $script:statusAbsent=$true
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'Failed preflight changed the files or receipt.'
    $script:entries[$paths.RegistryPath]=@{UninstallString='foreign changed entry'}
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'A changed Apps entry was accepted.'
    Require-Test (-not(Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $receipt.Path)) 'Changed Apps entry caused a file restoration.'
    $script:entries[$paths.RegistryPath]=$entry
    [IO.File]::WriteAllText($missing,'foreign content')
    Reject-Test {Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'} 'A changed destination was overwritten.'
    Require-Test ([IO.File]::ReadAllText($missing) -ceq 'foreign content') 'Recovery changed an unknown file.'
    [IO.File]::Delete($missing)
    Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'
    Require-Test ((Test-Path -LiteralPath $missing) -and -not(Test-Path -LiteralPath $receipt.Path)) 'Retry after rejected recovery did not succeed.'
    # Simulate interruption after the entire product and maintenance trees
    # were removed, but before the receipt could be cleared.
    $receipt=Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $entry -Maintenance $maintenance -ExpectedPlanHash $expectedPlanHash -ExpectedMaintenanceManifestHash $expectedMaintenanceHash
    foreach($row in $proof.files) {[IO.File]::Delete((Join-Path $root $row.relative_path.Replace('/','\')))}
    foreach($directory in @('versions\r61\x64','versions\r61','versions','shared')) {
        [IO.Directory]::Delete((Join-Path $root $directory))
    }
    [IO.Directory]::Delete($root)
    [IO.File]::Delete($paths.Executable);[IO.File]::Delete((Join-Path $paths.MaintenanceRoot 'maintenance.json'))
    [IO.Directory]::Delete($paths.MaintenanceRoot);$script:entries.Remove($paths.RegistryPath)
    Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'
    Require-Test ((Test-Path -LiteralPath $root) -and (Test-Path -LiteralPath $paths.Executable) -and
        (Test-WuGaiEntriesEqual (Read-WuGaiAppsEntry $paths) $entry) -and -not(Test-Path -LiteralPath $receipt.Path)) 'Full removal checkpoint was not recoverable.'
    Assert-WuGaiUninstallInventory $root $proof
    [IO.File]::Delete($paths.Executable);[IO.File]::Delete((Join-Path $paths.MaintenanceRoot 'maintenance.json'))
    [IO.Directory]::Delete($paths.MaintenanceRoot);$script:entries.Remove($paths.RegistryPath)
    $receipt=Write-WuGaiRemovalReceipt -Root $root -Sid $testSid -Backup $backup -AppsEntry $null -Maintenance $null -ExpectedPlanHash $expectedPlanHash
    [IO.File]::Delete($missing)
    $otherMissing=Join-Path $root 'versions\r61\x64\BoshiamyTsf.dll'
    [IO.File]::Delete($otherMissing)
    $ready=Join-Path $testRoot 'recovery-crash-ready.txt'
    $restartScript=Join-Path $PSScriptRoot 'uninstall-file-recovery-restart-fixture.ps1'
    $childStdout=Join-Path $testRoot 'recovery-child.stdout.txt';$childStderr=Join-Path $testRoot 'recovery-child.stderr.txt'
    $oldModulePath=$env:PSModulePath
    try {
        $env:PSModulePath=(Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules')+';'+$oldModulePath
        $child=Start-Process -FilePath $powershell -ArgumentList @('-NoLogo','-NoProfile','-NonInteractive',
            '-ExecutionPolicy','Bypass','-File',('"'+$restartScript+'"'),'-FixtureRoot',('"'+$root+'"'),
            '-ReceiptPath',('"'+$receipt.Path+'"'),'-ReadyFile',('"'+$ready+'"'),'-FixtureSid',$testSid) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $childStdout -RedirectStandardError $childStderr
    } finally { $env:PSModulePath=$oldModulePath }
    try {
        $deadline=[DateTime]::UtcNow.AddSeconds(12)
        while (-not (Test-Path -LiteralPath $ready) -and [DateTime]::UtcNow -lt $deadline) {
            if($child.HasExited){throw ('Recovery fixture exited before its checkpoint: '+(Get-Content -LiteralPath $childStderr -Raw))}
            Start-Sleep -Milliseconds 100
        }
        Require-Test (Test-Path -LiteralPath $ready) 'Recovery fixture did not reach its copy checkpoint.'
        $child.Kill();$child.WaitForExit(5000)|Out-Null
        Require-Test ($child.HasExited -and (Test-Path -LiteralPath $receipt.Path) -and
            ((Test-Path -LiteralPath $missing) -xor (Test-Path -LiteralPath $otherMissing))) 'Recovery interruption did not preserve a partial restore and receipt.'
    } finally {
        if(-not $child.HasExited){$child.Kill();$child.WaitForExit(5000)|Out-Null}
        $child.Dispose()
    }
    Recover-WuGaiInterruptedFileRemoval $root $testSid 'fixture-only'
    Require-Test ((Test-Path -LiteralPath $missing) -and (Test-Path -LiteralPath $otherMissing) -and -not(Test-Path -LiteralPath $paths.MaintenanceRoot) -and
        $null -eq (Read-WuGaiAppsEntry $paths) -and -not(Test-Path -LiteralPath $receipt.Path)) 'Recovery created a maintenance entry that did not previously exist.'
    Write-Output ('uninstall_file_recovery_tests_passed='+$script:passed+'; registry=fake; installer_invocations=0')
} finally { $env:LOCALAPPDATA=$oldLocalAppData }
