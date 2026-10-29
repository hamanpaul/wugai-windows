$ErrorActionPreference = 'Stop'
$uninstallTestRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$uninstallTestRoot = Join-Path $uninstallTestRepo ('build\uninstall-validator-tests-' + [guid]::NewGuid().ToString('N'))
$uninstallTestInstall = Join-Path $uninstallTestRoot 'Installed WuGai'
$uninstallTestSid = 'S-1-5-21-111111111-222222222-333333333-1001'
$uninstallTestUtf8 = New-Object Text.UTF8Encoding($false)
$script:uninstallTestPassed = 0
function Require-UninstallTest([bool]$Value, [string]$Message) {
    if (-not $Value) { throw $Message }
    $script:uninstallTestPassed++
}
function Reject-UninstallTest([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require-UninstallTest $rejected $Message
}
function Clone-UninstallReport($Value) { return ($Value | ConvertTo-Json -Depth 12 | ConvertFrom-Json) }

# Only load side-effect-free helpers, then extract validators and the file-only
# recovery helper by AST.
# The uninstall script's main flow, executable launcher and exit statements are
# never invoked. Fail explicitly if a validator crosses a registry boundary.
. (Join-Path $uninstallTestRepo 'native\maintenance.ps1')
function Read-WuGaiAppsEntry { throw 'Unexpected real Apps access in validator tests.' }
function Write-WuGaiAppsEntry { throw 'Unexpected real Apps write in validator tests.' }
function Remove-WuGaiAppsEntry { throw 'Unexpected real Apps removal in validator tests.' }
$tokens = $null; $parseErrors = $null
$source = Join-Path $uninstallTestRepo 'native\uninstall-release.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
Require-UninstallTest ($parseErrors.Count -eq 0) 'Uninstall script did not parse.'
$sharedTokens = $null; $sharedErrors = $null
$sharedSource = Join-Path $uninstallTestRepo 'native\uninstall-file-recovery.ps1'
$sharedAst = [Management.Automation.Language.Parser]::ParseFile($sharedSource, [ref]$sharedTokens, [ref]$sharedErrors)
Require-UninstallTest ($sharedErrors.Count -eq 0) 'Shared uninstall-file helpers did not parse.'
foreach ($name in @('Assert-WuGaiUninstallInventory','Assert-WuGaiAbsentReport','Assert-WuGaiUninstallReport','Restore-WuGaiUninstallFiles',
    'ConvertFrom-WuGaiModuleProcessList','Get-WuGaiMappedModuleUsers','Assert-WuGaiModuleNotMapped')) {
    $sourceAst = if ($name -cin @('ConvertFrom-WuGaiModuleProcessList','Get-WuGaiMappedModuleUsers','Assert-WuGaiModuleNotMapped')) { $ast } else { $sharedAst }
    $functions = @($sourceAst.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $false))
    Require-UninstallTest ($functions.Count -eq 1) "Expected one validator definition: $name"
    . ([scriptblock]::Create($functions[0].Extent.Text))
}

# Simulate module inventory without touching processes or executing tasklist.
$candidateLines = @('"explorer.exe","1234","BoshiamyTsf.dll"', '"explorer.exe","1234","BoshiamyTsf.dll"',
    '"ChatGPT.exe","2345","BoshiamyTsf.dll"')
$parsedCandidates = @(ConvertFrom-WuGaiModuleProcessList $candidateLines)
Require-UninstallTest ($parsedCandidates.Count -eq 2 -and $parsedCandidates[0].Id -eq 1234 -and $parsedCandidates[1].Id -eq 2345) 'Mapped-process inventory was not parsed or deduplicated.'
Reject-UninstallTest { ConvertFrom-WuGaiModuleProcessList @('"broken.exe","not-a-pid","BoshiamyTsf.dll"') } 'Invalid process ID was accepted.'
Require-UninstallTest (@(ConvertFrom-WuGaiModuleProcessList @('INFO: no matching processes')).Count -eq 0) 'Empty module inventory was not accepted.'
$script:moduleTestCandidates = $parsedCandidates
$script:moduleTestPaths = @{}
$script:moduleTestInaccessible = @{}
function Get-WuGaiModuleProcessCandidates { return $script:moduleTestCandidates }
function Get-WuGaiProcessModulePaths([int]$ProcessId) {
    if ($script:moduleTestInaccessible.ContainsKey($ProcessId)) { throw 'Fixture process is inaccessible.' }
    return $script:moduleTestPaths[$ProcessId]
}
$targetDll = Join-Path $uninstallTestInstall 'versions\r61\x64\BoshiamyTsf.dll'
$script:moduleTestPaths[1234] = @($targetDll.ToUpperInvariant())
$script:moduleTestPaths[2345] = @('C:\Other\BoshiamyTsf.dll')
$users = @(Get-WuGaiMappedModuleUsers @($targetDll))
Require-UninstallTest ($users.Count -eq 1 -and $users[0].Id -eq 1234 -and $users[0].Inspection -ceq 'matched') 'Mapped-module check did not distinguish the installed DLL from another copy.'
$modulePlan = [pscustomobject]@{files=@([pscustomobject]@{relative_path='versions/r61/x64/BoshiamyTsf.dll'})}
$moduleError = ''
try { Assert-WuGaiModuleNotMapped $modulePlan $uninstallTestInstall } catch { $moduleError = $_.Exception.Message }
Require-UninstallTest ($moduleError -match 'explorer\.exe \(PID 1234, matched\)' -and $moduleError -match 'no registrations were removed') 'The blocked-uninstall message did not identify the matching process.'
$sourceText = [IO.File]::ReadAllText($source)
$firstCheck = $sourceText.IndexOf('    Assert-WuGaiModuleNotMapped $plan $root')
$secondCheck = $sourceText.IndexOf('    Assert-WuGaiModuleNotMapped $plan $root', $firstCheck + 1)
$exclusiveOpen = $sourceText.IndexOf('[IO.File]::Open($source,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)')
$unregister = $sourceText.IndexOf("'--uninstall-package-registration'")
Require-UninstallTest ($firstCheck -ge 0 -and $firstCheck -lt $exclusiveOpen -and $secondCheck -gt $exclusiveOpen -and $secondCheck -lt $unregister) 'Mapped-module checks must bracket backup preparation before registration removal.'
$script:moduleTestInaccessible[2345] = $true
$users = @(Get-WuGaiMappedModuleUsers @($targetDll))
Require-UninstallTest ($users.Count -eq 2 -and @($users | Where-Object { $_.Id -eq 2345 -and $_.Inspection -ceq 'unavailable' }).Count -eq 1) 'Inaccessible module inventory was not treated conservatively.'
$script:moduleTestInaccessible.Remove(2345)
$script:moduleTestPaths[2345] = @()
$users = @(Get-WuGaiMappedModuleUsers @($targetDll))
Require-UninstallTest ($users.Count -eq 2 -and @($users | Where-Object { $_.Id -eq 2345 -and $_.Inspection -ceq 'unavailable' }).Count -eq 1) 'A missing process-module result was not treated conservatively.'
Reject-UninstallTest { Get-WuGaiMappedModuleUsers @('relative\BoshiamyTsf.dll') } 'Relative DLL path was accepted.'
Reject-UninstallTest { Get-WuGaiMappedModuleUsers @() } 'Missing DLL inventory was accepted.'

New-Item -ItemType Directory -Path $uninstallTestInstall -Force | Out-Null
$rows = @()
foreach ($relative in @('.liukai-owner','LiuKaiInstaller.exe','shared/branding.dll','versions/r61/x64/BoshiamyTsf.dll')) {
    $path = Join-Path $uninstallTestInstall $relative.Replace('/','\')
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, "fixture:$relative", $uninstallTestUtf8)
    $rows += [pscustomobject]@{relative_path=$relative; sha256=(Get-WuGaiHash $path); bytes=(Get-Item -LiteralPath $path).Length}
}
# The fixture list above is in protocol ordinal order. Hash a reference file
# independently, then send its rows out of order to exercise canonicalization.
$referenceImage = "liukai-uninstall-inventory-v1`n"
foreach ($row in $rows) { $referenceImage += $row.relative_path + "`t" + $row.sha256 + "`n" }
$referencePath = Join-Path $uninstallTestRoot 'inventory-reference.txt'
[IO.File]::WriteAllText($referencePath, $referenceImage, $uninstallTestUtf8)
$proof = [pscustomobject]@{schema_version=1; executed=$false; install_root=$uninstallTestInstall; user_sid=$uninstallTestSid;
    inventory_fingerprint=(Get-WuGaiHash $referencePath); manifest_sha256=('b' * 64);
    files=@($rows[3],$rows[1],$rows[0],$rows[2]); registrations_absent=$false}
$planned = [pscustomobject]@{kind='windows-installer-lifecycle'; operation=0; cleanup=0; readback=0; package_uninstall=$proof}
$verified = Assert-WuGaiUninstallReport $planned $uninstallTestInstall $uninstallTestSid $false
Require-UninstallTest ($verified.install_root -ceq $uninstallTestInstall -and $verified.files.Count -eq 4) 'Valid preflight proof was not returned.'
Assert-WuGaiUninstallInventory $uninstallTestInstall $proof
$script:uninstallTestPassed++
$executed = Clone-UninstallReport $planned
$executed.package_uninstall.executed = $true
$executed.package_uninstall.registrations_absent = $true
Assert-WuGaiUninstallReport $executed $uninstallTestInstall $uninstallTestSid $true | Out-Null
$script:uninstallTestPassed++

foreach ($field in @('operation','cleanup','readback')) {
    $bad = Clone-UninstallReport $planned
    $bad.$field = 2147942413
    Reject-UninstallTest { Assert-WuGaiUninstallReport $bad $uninstallTestInstall $uninstallTestSid $false } "Accepted failed $field."
}
foreach ($field in @('install_root','user_sid','inventory_fingerprint','manifest_sha256','schema_version')) {
    $bad = Clone-UninstallReport $planned
    $bad.package_uninstall.$field = if ($field -eq 'schema_version') { 2 } else { 'mismatched-value' }
    Reject-UninstallTest { Assert-WuGaiUninstallReport $bad $uninstallTestInstall $uninstallTestSid $false } "Accepted mismatched proof $field."
}
$bad = Clone-UninstallReport $planned; $bad.package_uninstall.files = @()
Reject-UninstallTest { Assert-WuGaiUninstallReport $bad $uninstallTestInstall $uninstallTestSid $false } 'Accepted an empty uninstall inventory.'
$bad = Clone-UninstallReport $planned; $bad.package_uninstall = $null
Reject-UninstallTest { Assert-WuGaiUninstallReport $bad $uninstallTestInstall $uninstallTestSid $false } 'Accepted a missing uninstall proof.'
Reject-UninstallTest { Assert-WuGaiUninstallReport $planned $uninstallTestInstall $uninstallTestSid $true } 'Accepted a plan as executed proof.'
$bad = Clone-UninstallReport $executed; $bad.package_uninstall.registrations_absent = $false
Reject-UninstallTest { Assert-WuGaiUninstallReport $bad $uninstallTestInstall $uninstallTestSid $true } 'Accepted execution without registration absence.'

$badProof = Clone-UninstallReport $proof; $badProof.install_root = $uninstallTestRoot
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $badProof } 'Inventory accepted a different root.'
$badProof = Clone-UninstallReport $proof; $badProof.inventory_fingerprint = '0' * 64
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $badProof } 'Inventory accepted a different fingerprint.'
$extra = Join-Path $uninstallTestInstall 'foreign.txt'
[IO.File]::WriteAllText($extra, 'retain me', $uninstallTestUtf8)
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $proof } 'Inventory accepted an unknown file.'
Require-UninstallTest ([IO.File]::ReadAllText($extra) -ceq 'retain me') 'Inventory validator modified the unknown file.'
[IO.File]::Delete($extra)
$emptyDirectory = Join-Path $uninstallTestInstall 'unknown-empty-directory'
[IO.Directory]::CreateDirectory($emptyDirectory) | Out-Null
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $proof } 'Inventory accepted an unknown empty directory.'
[IO.Directory]::Delete($emptyDirectory)
$changedFile = Join-Path $uninstallTestInstall 'shared\branding.dll'
$originalBytes = [IO.File]::ReadAllBytes($changedFile)
$changedBytes = [byte[]]$originalBytes.Clone(); $changedBytes[0] = $changedBytes[0] -bxor 1
[IO.File]::WriteAllBytes($changedFile, $changedBytes)
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $proof } 'Inventory accepted same-length hash drift.'
[IO.File]::WriteAllBytes($changedFile, $originalBytes)
$badProof = Clone-UninstallReport $proof; $badProof.files = @($badProof.files[0],$badProof.files[0])
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $badProof } 'Inventory accepted duplicate paths.'
foreach ($badPath in @('../escape','/absolute','shared//branding.dll','shared/','C:/outside')) {
    $badProof = Clone-UninstallReport $proof; $badProof.files[0].relative_path = $badPath
    Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $badProof } "Inventory accepted unsafe path $badPath."
}

# Simulate program-file removal interrupted partway through. The backup and
# every restoration target are inside this test's build directory.
$backupRoot = Join-Path $uninstallTestRoot 'validated backup'
foreach ($row in $proof.files) {
    $relative = $row.relative_path.Replace('/','\')
    $backupPath = Join-Path $backupRoot $relative
    [IO.Directory]::CreateDirectory((Split-Path -Parent $backupPath)) | Out-Null
    [IO.File]::Copy((Join-Path $uninstallTestInstall $relative), $backupPath)
}
$restoreRoot = Join-Path $uninstallTestRoot 'Restored WuGai'
$restoreProof = Clone-UninstallReport $proof; $restoreProof.install_root = $restoreRoot
Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot
Assert-WuGaiUninstallInventory $restoreRoot $restoreProof
$script:uninstallTestPassed++
$missingPath = Join-Path $restoreRoot 'shared\branding.dll'
$retainedPath = Join-Path $restoreRoot 'LiuKaiInstaller.exe'
$retainedTime = (Get-Item -LiteralPath $retainedPath).LastWriteTimeUtc
[IO.File]::Delete($missingPath)
Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot
Require-UninstallTest ((Get-WuGaiHash $missingPath) -ceq ($rows | Where-Object relative_path -ceq 'shared/branding.dll').sha256) 'Recovery did not restore a missing file.'
Require-UninstallTest ((Get-Item -LiteralPath $retainedPath).LastWriteTimeUtc -eq $retainedTime) 'Recovery rewrote an already verified file.'
Assert-WuGaiUninstallInventory $restoreRoot $restoreProof
$script:uninstallTestPassed++
$recoveryForeign = Join-Path $restoreRoot 'foreign.txt'
[IO.File]::WriteAllText($recoveryForeign, 'foreign recovery sentinel', $uninstallTestUtf8)
[IO.File]::Delete($missingPath)
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot } 'Recovery accepted an unknown target file.'
Require-UninstallTest (-not (Test-Path -LiteralPath $missingPath) -and [IO.File]::ReadAllText($recoveryForeign) -ceq 'foreign recovery sentinel') 'Rejected recovery modified the target.'
[IO.File]::Delete($recoveryForeign)
$recoveryDirectory = Join-Path $restoreRoot 'unknown empty directory'
[IO.Directory]::CreateDirectory($recoveryDirectory) | Out-Null
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot } 'Recovery accepted an unknown target directory.'
Require-UninstallTest (-not (Test-Path -LiteralPath $missingPath)) 'Directory rejection restored files prematurely.'
[IO.Directory]::Delete($recoveryDirectory)
$retainedBytes = [IO.File]::ReadAllBytes($retainedPath)
[IO.File]::WriteAllText($retainedPath, 'changed existing target', $uninstallTestUtf8)
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot } 'Recovery overwrote a changed target.'
Require-UninstallTest ([IO.File]::ReadAllText($retainedPath) -ceq 'changed existing target' -and -not (Test-Path -LiteralPath $missingPath)) 'Changed-target rejection mutated files.'
[IO.File]::WriteAllBytes($retainedPath, $retainedBytes)
$backupFile = Join-Path $backupRoot 'shared\branding.dll'
$backupBytes = [IO.File]::ReadAllBytes($backupFile)
[IO.File]::WriteAllText($backupFile, 'corrupt backup', $uninstallTestUtf8)
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot } 'Recovery accepted a corrupt backup.'
Require-UninstallTest (-not (Test-Path -LiteralPath $missingPath)) 'Corrupt-backup rejection wrote the missing file.'
[IO.File]::WriteAllBytes($backupFile, $backupBytes)
$wrongRestore = Clone-UninstallReport $restoreProof; $wrongRestore.install_root = $uninstallTestInstall
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $wrongRestore $backupRoot } 'Recovery accepted a mismatched root.'
Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot
Assert-WuGaiUninstallInventory $restoreRoot $restoreProof
$script:uninstallTestPassed++

$junctionTarget = Join-Path $uninstallTestRoot 'junction-target'
[IO.Directory]::CreateDirectory($junctionTarget) | Out-Null
[IO.File]::WriteAllText((Join-Path $junctionTarget 'sentinel.txt'), 'untouched target', $uninstallTestUtf8)
New-Item -ItemType Junction -Path (Join-Path $uninstallTestInstall 'redirect') -Target $junctionTarget | Out-Null
Reject-UninstallTest { Assert-WuGaiUninstallInventory $uninstallTestInstall $proof } 'Inventory followed a junction.'
Require-UninstallTest ([IO.File]::ReadAllText((Join-Path $junctionTarget 'sentinel.txt')) -ceq 'untouched target') 'Inventory validator modified the junction target.'
New-Item -ItemType Junction -Path (Join-Path $restoreRoot 'redirect') -Target $junctionTarget | Out-Null
Reject-UninstallTest { Restore-WuGaiUninstallFiles $restoreRoot $restoreProof $backupRoot } 'Recovery followed a target junction.'
Require-UninstallTest ([IO.File]::ReadAllText((Join-Path $junctionTarget 'sentinel.txt')) -ceq 'untouched target') 'Rejected recovery modified its junction target.'

$resources = @()
foreach ($name in @('com_x64','com_x86','profile','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')) {
    $resource = [ordered]@{name=$name; read_result=0; present=$false; lifecycle_owned=$false}
    if ($name -ceq 'com-machine-x64') { $resource.scope='machine'; $resource.hive='HKLM'; $resource.registry_view=64 }
    $resources += [pscustomobject]$resource
}
$absent = [pscustomobject]@{kind='windows-installer-lifecycle'; operation=0; cleanup=0; readback=0;
    profile_present_result=0; profile_present=$false; resources=$resources;
    all_categories_result=0; all_categories_count=0; profile_registry_absence_result=0}
Assert-WuGaiAbsentReport $absent
$script:uninstallTestPassed++
foreach ($field in @('operation','cleanup','readback','profile_present_result','all_categories_result','profile_registry_absence_result')) {
    $bad = Clone-UninstallReport $absent; $bad.$field = 2147942405
    Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } "Absence accepted failed $field."
}
foreach ($field in @('all_categories_result','all_categories_count','profile_registry_absence_result')) {
    $bad = Clone-UninstallReport $absent; $bad.PSObject.Properties.Remove($field)
    Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } "Absence accepted missing $field."
}
$bad = Clone-UninstallReport $absent; $bad.all_categories_count = 1
Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } 'Absence accepted a remaining modern-host category.'
$bad = Clone-UninstallReport $absent; $bad.profile_present = $true
Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } 'Absence accepted an existing profile.'
foreach ($field in @('present','lifecycle_owned','read_result')) {
    $bad = Clone-UninstallReport $absent
    $bad.resources[0].$field = if ($field -ceq 'read_result') { 1 } else { $true }
    Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } "Absence accepted invalid resource $field."
}
$bad = Clone-UninstallReport $absent; $bad.resources = @($bad.resources[0])
Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } 'Absence accepted an incomplete resource list.'
$bad = Clone-UninstallReport $absent; $bad.resources[6].name = 'com_x64'
Reject-UninstallTest { Assert-WuGaiAbsentReport $bad } 'Absence accepted duplicate resources hiding machine COM.'
Write-Output "uninstall_validator_tests_passed=$script:uninstallTestPassed; main_flow_executed=false; registry_accesses=0; executable_invocations=0"
