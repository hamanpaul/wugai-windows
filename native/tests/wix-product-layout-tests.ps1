param([Parameter(Mandatory=$true)][string]$Msi,[switch]$Installable)
$ErrorActionPreference='Stop'
$path=(Resolve-Path -LiteralPath $Msi).Path
$db=(New-Object -ComObject WindowsInstaller.Installer).OpenDatabase($path,0)
$view=$db.OpenView('SELECT `FileName` FROM `File`');$view.Execute();$actual=@()
while($row=$view.Fetch()){$parts=$row.StringData(1).Split('|');$actual+= $parts[$parts.Length-1]}
$expected=@('LiuKaiInstaller.exe','LiuKaiRegister.exe','branding.dll','package-manifest.json','BoshiamyTsf.dll','LiuKaiLookup.exe','LiuKaiSettings.exe','dictionary.tsv','readings.tsv','recommended-codes.tsv','recommended-codes-source.json','phonetic-candidates.tsv','phonetic-candidates-source.json','THIRD_PARTY_NOTICES.md','recover-legacy-profile.ps1','legacy-prepared-root.ps1','legacy-recovery-policy.ps1','legacy-recovery-observe.ps1','legacy-recovery-transaction.ps1','legacy-recovery-adapter.ps1','legacy-recovery-backup.ps1','legacy-recovery-registry.ps1','legacy-owner-marker.ps1','legacy-history-probe.exe','collect-install-diagnostics.ps1')
if($actual.Count -ne $expected.Count -or @($actual|Select-Object -Unique).Count -ne $expected.Count){throw 'Unexpected MSI file count or duplicate names.'}
foreach($name in $expected){if($name -cnotin $actual){throw "Expected MSI payload file missing: $name"}}
$view=$db.OpenView('SELECT `Condition` FROM `LaunchCondition`');$view.Execute();$conditions=@()
while($row=$view.Fetch()){$conditions += $row.StringData(1)}
if($Installable){if($conditions.Count -ne 2 -or 'VersionNT64' -cnotin $conditions -or 'NOT WIX_DOWNGRADE_DETECTED' -cnotin $conditions){throw 'Installable test package must require 64-bit Windows.'}}
elseif('0' -cnotin $conditions){throw 'Development package must be blocked from installation until TSF and rollback are complete.'}
$view=$db.OpenView('SELECT * FROM `CustomAction`');$view.Execute();$actions=@{}
while($row=$view.Fetch()){$actions[$row.StringData(1)]=@($row.IntegerData(2),$row.StringData(3),$row.StringData(4))}
foreach($case in @(@('CollectInstallFailure',17729,'CollectFailure'),@('CollectRemovalFailure',17729,'CollectFailure'),@('CaptureUserContext',1),@('PrepareFreshRegistration',1),@('ValidateUserContext',17409),@('RollbackFresh',17665),@('RecoverLegacyOrphan',17409),@('RegisterFresh',17409),@('PrepareRemoval',1),@('UndoRemoval',17665),@('RemoveRegistration',17409))){
  $name=$case[0]
  if(-not $actions.ContainsKey($name) -or $actions[$name][0] -ne $case[1] -or $actions[$name][1] -cne 'UserContext' -or $actions[$name][2] -cne $(if($case.Count -eq 3){$case[2]}else{$name})){throw "Invalid user context action: $name"}
}
$view=$db.OpenView('SELECT * FROM `InstallExecuteSequence`');$view.Execute();$sequence=@{}
while($row=$view.Fetch()){$sequence[$row.StringData(1)]=[pscustomobject]@{condition=$row.StringData(2);order=$row.IntegerData(3)}}
if(-not ($sequence['InstallInitialize'].order -lt $sequence['CaptureUserContext'].order -and $sequence['CaptureUserContext'].order -lt $sequence['PrepareFreshRegistration'].order -and $sequence['PrepareFreshRegistration'].order -lt $sequence['ValidateUserContext'].order -and $sequence['ValidateUserContext'].order -lt $sequence['RollbackFresh'].order -and $sequence['InstallFiles'].order -lt $sequence['CollectInstallFailure'].order -and $sequence['CollectInstallFailure'].order -lt $sequence['RollbackFresh'].order -and $sequence['RollbackFresh'].order -lt $sequence['RecoverLegacyOrphan'].order -and $sequence['RecoverLegacyOrphan'].order -lt $sequence['RegisterFresh'].order -and $sequence['RegisterFresh'].order -lt $sequence['InstallFinalize'].order)){throw 'Invalid registration action order.'}
if(-not ($sequence['PrepareFreshRegistration'].order -lt $sequence['CreateFolders'].order -and $sequence['PrepareFreshRegistration'].order -lt $sequence['InstallFiles'].order -and $sequence['PrepareFreshRegistration'].order -lt $sequence['WriteRegistryValues'].order)){throw 'Fresh-install preflight must precede MSI directory, file and registry writes.'}
foreach($name in @('CaptureUserContext','PrepareFreshRegistration','ValidateUserContext','CollectInstallFailure','RollbackFresh','RecoverLegacyOrphan','RegisterFresh')){if($sequence[$name].condition -cne 'NOT Installed'){throw "Invalid registration action condition: $name"}}
if(-not ($sequence['InstallInitialize'].order -lt $sequence['PrepareRemoval'].order -and $sequence['PrepareRemoval'].order -lt $sequence['CollectRemovalFailure'].order -and $sequence['CollectRemovalFailure'].order -lt $sequence['UndoRemoval'].order -and $sequence['UndoRemoval'].order -lt $sequence['RemoveRegistration'].order -and $sequence['RemoveRegistration'].order -lt $sequence['RemoveRegistryValues'].order -and $sequence['RemoveRegistration'].order -lt $sequence['RemoveFiles'].order)){throw 'Invalid removal action order.'}
foreach($name in @('PrepareRemoval','CollectRemovalFailure','UndoRemoval','RemoveRegistration')){if($sequence[$name].condition -cne 'REMOVE="ALL"'){throw "Invalid removal action condition: $name"}}
$view=$db.OpenView('SELECT `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry`');$view.Execute();$identity=@{}
while($row=$view.Fetch()){
  if($row.StringData(5) -ceq 'InstallIdentity'){
    if($row.IntegerData(1) -ne 2 -or $row.StringData(2) -cne 'Software\LiuKai\MSI\{7E8341DC-CE5A-4A3A-A6BC-2C5E70FEFC39}'){throw 'Unexpected installation identity registry location.'}
    $identity[$row.StringData(3)]=$row.StringData(4)
  }
}
if($identity.Count -ne 5 -or $identity['Owner'] -cne 'LiuKai-TSF-v1' -or $identity['Token'] -cne '[LIUKAIINSTALLTOKEN]' -or $identity['Sid'] -cne '[LIUKAIINSTALLSID]' -or $identity['ManifestSha256'] -cnotmatch '^[0-9a-f]{64}$' -or !$identity['Version']){throw 'Invalid MSI identity metadata.'}
$view=$db.OpenView('SELECT `Property`, `Signature_` FROM `AppSearch`');$view.Execute();$search=@{}
while($row=$view.Fetch()){$search[$row.StringData(1)]=$row.StringData(2)}
if($search['LIUKAIINSTALLTOKEN'] -cne 'FindInstallToken' -or $search['LIUKAIINSTALLSID'] -cne 'FindInstallSid'){throw 'Missing MSI identity lookups.'}
if($Installable){'product_layout_files_verified=25;installable_test_requires_x64=true'}else{'product_layout_files_verified=25;development_install_blocked=true'}
