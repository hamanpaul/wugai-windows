$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-policy.ps1')
$names=@('com_x64','com_x86','keyboard_category','system_tray_category','ui_element_category','com-machine-x64')
$rows=@($names | ForEach-Object { [pscustomobject]@{name=$_;read_result=0;present=$false;lifecycle_owned=$false} })
$rows+=@([pscustomobject]@{name='profile';read_result=0;present=$false;lifecycle_owned=$true})
$state=[pscustomobject]@{IsElevated=$true;UserSid='sid';ExpectedUserSid='sid';HistoryExit=0;HistoryInspection=0;LegacyHistoryInspection=0;PendingFileRemoval=$false;PendingMaintenancePublication=$false;InstallRootExists=$false;MaintenanceRootExists=$false;StatusExit=1;Status=[pscustomobject]@{kind='windows-installer-lifecycle';scope='user-and-machine-com-x64';operation=0;cleanup=0;readback=2147943706;profile_registry_absence_result=2147943706;profile_present_result=0;profile_present=$false;all_categories_result=0;all_categories_count=0;resources=$rows};ProfileSubkeys=0;ProfileValueCount=1;ProfileValueName='Enable';ProfileValueKind='DWord';ProfileEnable=1;MarkerSubkeys=0;MarkerValueCount=1;MarkerValueName='Record';MarkerValueKind='Binary';MarkerOwner='LiuKai-TSF-v1';MarkerIcon='C:\Program Files\LiuKai\shared\branding.dll';ExpectedIcon='C:\Program Files\LiuKai\shared\branding.dll';MarkerEnabled=1;MarkerEncodingValid=$true}
Assert-LegacyProfileRecoveryPreconditions $state
foreach($case in @(@('HistoryInspection',1),@('LegacyHistoryInspection',1),@('PendingFileRemoval',$true),@('PendingFileRemoval',$null),@('InstallRootExists',$null),@('PendingMaintenancePublication',$true),@('UserSid','other'),@('InstallRootExists',$true),@('ProfileSubkeys',1),@('MarkerOwner','foreign'),@('MarkerEncodingValid',$false),@('StatusExit',0))) {
    $field=$case[0];$old=$state.$field;$state.$field=$case[1];$rejected=$false
    try { Assert-LegacyProfileRecoveryPreconditions $state } catch { $rejected=$true } finally { $state.$field=$old }
    if(-not $rejected){throw "Unsafe recovery case accepted: $field"}
}
$original=$state.Status.resources; $state.Status.resources=@($original)+@([pscustomobject]@{name='unknown'}); $rejected=$false
try {Assert-LegacyProfileRecoveryPreconditions $state}catch{$rejected=$true}
if(-not $rejected){throw 'Unknown resource accepted'}; $state.Status.resources=$original
$state.Status.resources[0].present=$true;$rejected=$false
try {Assert-LegacyProfileRecoveryPreconditions $state}catch{$rejected=$true}
if(-not $rejected){throw 'Present COM registration accepted'}
$state.Status.resources[0].present=$false
$state | Add-Member -NotePropertyName MachineProfileExists -NotePropertyValue $false
Assert-LegacyResumePreconditions $state
foreach($case in @(@('MachineProfileExists',$true),@('LegacyHistoryInspection',1),@('PendingFileRemoval',$null))) {
  $field=$case[0];$old=$state.$field;$state.$field=$case[1];$rejected=$false
  try {Assert-LegacyResumePreconditions $state}catch{$rejected=$true}finally{$state.$field=$old}
  if(-not $rejected){throw "Unsafe resume case accepted: $field"}
}
$state.Status.readback=5;$rejected=$false;try{Assert-LegacyResumePreconditions $state}catch{$rejected=$true};if(-not $rejected){throw 'Unexpected status accepted for resume'}
$state.StatusExit=0;$state.Status.readback=0;$state.Status.profile_registry_absence_result=0;$state.Status.resources[-1].lifecycle_owned=$false
Assert-LegacyResumePreconditions $state
$state.Status.resources[-1].lifecycle_owned=$null;$rejected=$false;try{Assert-LegacyResumePreconditions $state}catch{$rejected=$true};if(-not $rejected){throw 'Unknown ownership accepted for resume'}
'legacy_recovery_policy_tests_passed=22'
