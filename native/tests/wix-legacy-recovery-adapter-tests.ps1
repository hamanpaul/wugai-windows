$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-policy.ps1')
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-transaction.ps1')
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-adapter.ps1')
$testName='LiuKaiRecoveryAdapterTest_'+[Guid]::NewGuid().ToString('N')
$software=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software',$true)
$root=$software.CreateSubKey($testName);$profiles=$root.CreateSubKey('profiles');$markers=$root.CreateSubKey('markers')
$backup=Join-Path $env:TEMP ('liukai-adapter-test-'+[Guid]::NewGuid().ToString('N'));[IO.Directory]::CreateDirectory($backup)|Out-Null
$leaf='{7254A21F-6EB1-4217-A42C-C27301175D90}';$marker=[Convert]::FromBase64String('AQAAAA0AAABMAGkAdQBLAGEAaQAtAFQAUwBGAC0AdgAxACsAAABDADoAXABQAHIAbwBnAHIAYQBtACAARgBpAGwAZQBzAFwATABpAHUASwBhAGkAXABzAGgAYQByAGUAZABcAGIAcgBhAG4AZABpAG4AZwAuAGQAbABsAAEAAAA=')
$names=@('com_x64','com_x86','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')
$rows=@($names | ForEach-Object {[pscustomobject]@{name=$_;read_result=0;present=$false;lifecycle_owned=$false}})+@([pscustomobject]@{name='profile';read_result=0;present=$false;lifecycle_owned=$true})
$state=[pscustomobject]@{IsElevated=$true;UserSid='sid';ExpectedUserSid='sid';HistoryExit=0;HistoryInspection=0;LegacyHistoryInspection=0;PendingFileRemoval=$false;PendingMaintenancePublication=$false;InstallRootExists=$false;MaintenanceRootExists=$false;StatusExit=1;Status=[pscustomobject]@{kind='windows-installer-lifecycle';scope='user-and-machine-com-x64';operation=0;cleanup=0;readback=2147943706;profile_registry_absence_result=2147943706;profile_present_result=0;profile_present=$false;all_categories_result=0;all_categories_count=0;resources=$rows};ProfileSubkeys=0;ProfileValueCount=1;ProfileValueName='Enable';ProfileValueKind='DWord';ProfileEnable=1;MarkerSubkeys=0;MarkerValueCount=1;MarkerValueName='Record';MarkerValueKind='Binary';MarkerOwner='LiuKai-TSF-v1';MarkerIcon='C:\Program Files\LiuKai\shared\branding.dll';ExpectedIcon='C:\Program Files\LiuKai\shared\branding.dll';MarkerEnabled=1;MarkerEncodingValid=$true;MarkerBytes=[Convert]::ToBase64String($marker)}
try {
  foreach($willFail in @($false,$true)) {
    $key=$profiles.CreateSubKey($leaf);$key.SetValue('Enable',1,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose()
    $key=$markers.CreateSubKey('Profile');$key.SetValue('Record',$marker,[Microsoft.Win32.RegistryValueKind]::Binary);$key.Dispose()
    $observer={ $state }.GetNewClosure();$postcheck={ -not $willFail }.GetNewClosure()
    $store=New-LegacyRecoveryStore -ProfileParent $profiles -MarkerParent $markers -BackupRoot $backup -Observer $observer -Postcheck $postcheck
    $failure=$false;$receipt=$null
    try {$receipt=Invoke-LegacyRecoveryTransaction $store}catch{$failure=$true;if(-not $willFail){throw}}
    if($failure -ne $willFail){throw 'Unexpected transaction outcome'}
    if($willFail){
      if((Test-LegacyRegistryLeaf $profiles $leaf 'Enable' 'DWord' 1) -cne 'matches' -or (Test-LegacyRegistryLeaf $markers 'Profile' 'Record' 'Binary' $marker) -cne 'matches'){throw 'Failed transaction did not restore'}
      Remove-LegacyRegistryLeafIfUnchanged $profiles $leaf 'Enable' 'DWord' 1; Remove-LegacyRegistryLeafIfUnchanged $markers 'Profile' 'Record' 'Binary' $marker
    } else {
      if(-not (Test-LegacyRecoveryBackup $receipt) -or -not (Test-Path -LiteralPath ($receipt.Path+'.complete'))){throw 'Success receipt missing'}
    }
  }
  # Simulate power loss after deleting only one leaf: reconstruct from the
  # durable receipt and refuse to continue if the scope cannot be confirmed.
  $key=$profiles.CreateSubKey($leaf);$key.SetValue('Enable',1,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose()
  $key=$markers.CreateSubKey('Profile');$key.SetValue('Record',$marker,[Microsoft.Win32.RegistryValueKind]::Binary);$key.Dispose()
  $observer={ $state }.GetNewClosure();$postcheck={ $true };$scope={ param($d) $true }
  $store=New-LegacyRecoveryStore -ProfileParent $profiles -MarkerParent $markers -BackupRoot $backup -Observer $observer -Postcheck $postcheck -ResumeScope $scope
  $fingerprint=Get-LegacyStateFingerprint $state
  $receipt=New-LegacyRecoveryBackup -Root $backup -Snapshot $state -Fingerprint $fingerprint
  Remove-LegacyRegistryLeafIfUnchanged $profiles $leaf 'Enable' 'DWord' 1
  $result=Resume-LegacyRecoveryTransaction $store $receipt
  if($result -cne 'restored' -or (Test-LegacyRegistryLeaf $profiles $leaf 'Enable' 'DWord' 1) -cne 'matches' -or -not (Test-Path -LiteralPath ($receipt.Path+'.restored'))){throw 'Interrupted recovery was not restored'}
  Remove-LegacyRegistryLeafIfUnchanged $profiles $leaf 'Enable' 'DWord' 1;Remove-LegacyRegistryLeafIfUnchanged $markers 'Profile' 'Record' 'Binary' $marker
  if(@(Get-LegacyRecoveryPendingReceipts -Root $backup -Store $store).Count -ne 0){throw 'Completed receipts were treated as pending'}
  $pending=New-LegacyRecoveryBackup -Root $backup -Snapshot $state -Fingerprint (Get-LegacyStateFingerprint $state)
  if(@(Get-LegacyRecoveryPendingReceipts -Root $backup -Store $store).Count -ne 1){throw 'Prepared receipt was not detected'}
  [IO.File]::WriteAllText((Join-Path $backup 'unexpected.txt'),'unexpected')
  $rejected=$false;try {$null=@(Get-LegacyRecoveryPendingReceipts -Root $backup -Store $store)} catch {$rejected=$true}
  if(-not $rejected){throw 'Unexpected recovery evidence accepted'}
  'legacy_recovery_adapter_tests_passed=6'
} finally { $profiles.Dispose();$markers.Dispose();$root.DeleteSubKey('profiles',$false);$root.DeleteSubKey('markers',$false);$root.Dispose();$software.DeleteSubKey($testName,$false);$software.Dispose() }
