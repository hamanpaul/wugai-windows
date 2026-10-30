param([Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-prepared-root.ps1')
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-policy.ps1')
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-transaction.ps1')
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-adapter.ps1')
$work=[IO.Path]::GetFullPath($WorkDir);if(Test-Path $work){throw 'Test directory exists.'};$root=Join-Path $work 'prepared';$dir=Join-Path $root 'versions\test-v1\x64';$support=Join-Path $root 'support';$backup=Join-Path $work 'backups';[IO.Directory]::CreateDirectory($dir)|Out-Null;[IO.Directory]::CreateDirectory($support)|Out-Null;[IO.Directory]::CreateDirectory($backup)|Out-Null
$file=Join-Path $dir 'BoshiamyTsf.dll';$extra=Join-Path $support 'helper.ps1';[IO.File]::WriteAllText($file,'payload');[IO.File]::WriteAllText($extra,'helper');$fileHash=(Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant();$extraHash=(Get-FileHash $extra -Algorithm SHA256).Hash.ToLowerInvariant()
$doc=[ordered]@{format='liukai-windows-package-v1';version='test-v1';architectures=@('x64');files=@(@{path='versions/test-v1/x64/BoshiamyTsf.dll';sha256=$fileHash;bytes=7})};$manifest=Join-Path $root 'versions\test-v1\package-manifest.json';[IO.File]::WriteAllText($manifest,($doc|ConvertTo-Json -Depth 5));$manifestHash=(Get-FileHash $manifest -Algorithm SHA256).Hash.ToLowerInvariant();$proof=@{'support/helper.ps1'=$extraHash}
$regName='LiuKaiPreparedRecoveryTest_'+[Guid]::NewGuid().ToString('N');if($regName -cnotmatch '^LiuKaiPreparedRecoveryTest_[0-9a-f]{32}$'){throw 'Invalid test registry name.'};$software=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software',$true);$existing=$software.OpenSubKey($regName,$false);if($existing){$existing.Dispose();$software.Dispose();throw 'Test registry key already exists.'};$sandbox=$software.CreateSubKey($regName);$profiles=$sandbox.CreateSubKey('profiles');$markers=$sandbox.CreateSubKey('markers');$leaf='{7254A21F-6EB1-4217-A42C-C27301175D90}';$marker=[byte[]](1,2,3)
$names=@('com_x64','com_x86','keyboard_category','system_tray_category','ui_element_category','com-machine-x64');$rows=@($names|ForEach-Object {[pscustomobject]@{name=$_;read_result=0;present=$false;lifecycle_owned=$false}})+@([pscustomobject]@{name='profile';read_result=0;present=$false;lifecycle_owned=$true})
$state=[pscustomobject]@{IsElevated=$true;UserSid='sid';ExpectedUserSid='sid';HistoryExit=0;HistoryInspection=0;LegacyHistoryInspection=0;PendingFileRemoval=$false;PendingMaintenancePublication=$false;InstallRootExists=$true;MaintenanceRootExists=$false;StatusExit=1;Status=[pscustomobject]@{kind='windows-installer-lifecycle';scope='user-and-machine-com-x64';operation=0;cleanup=0;readback=2147943706;profile_registry_absence_result=2147943706;profile_present_result=0;profile_present=$false;all_categories_result=0;all_categories_count=0;resources=$rows};ProfileSubkeys=0;ProfileValueCount=1;ProfileValueName='Enable';ProfileValueKind='DWord';ProfileEnable=1;MarkerSubkeys=0;MarkerValueCount=1;MarkerValueName='Record';MarkerValueKind='Binary';MarkerOwner='LiuKai-TSF-v1';MarkerIcon='expected';ExpectedIcon='expected';MarkerEnabled=1;MarkerEncodingValid=$true;MarkerBytes=[Convert]::ToBase64String($marker)}
try {
 $observer={if(-not (Assert-PreparedMsiRoot -Root $root -Version 'test-v1' -ManifestSha256 $manifestHash -ExtraHashes $proof)){throw 'Proof failed'};$copy=$state.PSObject.Copy();$copy.InstallRootExists=$false;return $copy}.GetNewClosure()
 $postcheck={$true};$store=New-LegacyRecoveryStore -ProfileParent $profiles -MarkerParent $markers -BackupRoot $backup -Observer $observer -Postcheck $postcheck
 foreach($tamper in @($true,$false)){
  $key=$profiles.CreateSubKey($leaf);$key.SetValue('Enable',1,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose();$key=$markers.CreateSubKey('Profile');$key.SetValue('Record',$marker,[Microsoft.Win32.RegistryValueKind]::Binary);$key.Dispose()
  if($tamper){[IO.File]::WriteAllText($extra,'altered')}
  $failed=$false;try{$receipt=Invoke-LegacyRecoveryTransaction $store}catch{$failed=$true;if(-not $tamper){throw}}
  if($failed -ne $tamper){throw 'Unexpected transaction outcome.'}
  if($tamper){if((Test-LegacyRegistryLeaf $profiles $leaf 'Enable' 'DWord' 1) -cne 'matches' -or (Test-LegacyRegistryLeaf $markers 'Profile' 'Record' 'Binary' $marker) -cne 'matches'){throw 'Tampered proof changed registry.'};Remove-LegacyRegistryLeafIfUnchanged $profiles $leaf 'Enable' 'DWord' 1;Remove-LegacyRegistryLeafIfUnchanged $markers 'Profile' 'Record' 'Binary' $marker;[IO.File]::WriteAllText($extra,'helper')}
  else{if(-not (Test-LegacyRecoveryBackup $receipt) -or -not (Test-Path ($receipt.Path+'.complete'))){throw 'Recovery backup missing.'};if((Test-LegacyRegistryLeaf $profiles $leaf 'Enable' 'DWord' 1) -cne 'absent' -or (Test-LegacyRegistryLeaf $markers 'Profile' 'Record' 'Binary' $marker) -cne 'absent'){throw 'Recovery did not remove test leaves.'}}
 }
 'wix_prepared_recovery_tests_passed=true'
} finally {
 if((Test-LegacyRegistryLeaf $profiles $leaf 'Enable' 'DWord' 1) -ceq 'matches'){Remove-LegacyRegistryLeafIfUnchanged $profiles $leaf 'Enable' 'DWord' 1}
 if((Test-LegacyRegistryLeaf $markers 'Profile' 'Record' 'Binary' $marker) -ceq 'matches'){Remove-LegacyRegistryLeafIfUnchanged $markers 'Profile' 'Record' 'Binary' $marker}
 $profiles.Dispose();$markers.Dispose();$sandbox.DeleteSubKey('profiles',$false);$sandbox.DeleteSubKey('markers',$false);$sandbox.Dispose();$software.DeleteSubKey($regName,$false);$software.Dispose()
}