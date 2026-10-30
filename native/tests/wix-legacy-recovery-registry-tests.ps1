$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-registry.ps1')
$testName='LiuKaiRecoveryTest_'+[Guid]::NewGuid().ToString('N')
$software=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software',$true)
$root=$software.CreateSubKey($testName)
try {
  $key=$root.CreateSubKey('profile');$key.SetValue('Enable',1,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose()
  if((Test-LegacyRegistryLeaf $root 'profile' 'Enable' 'DWord' 1) -cne 'matches'){throw 'expected profile mismatch'}
  $reject=$false;try{Remove-LegacyRegistryLeafIfUnchanged $root 'profile' 'Enable' 'DWord' 0}catch{$reject=$true};if(-not $reject){throw 'wrong value deleted'}
  Remove-LegacyRegistryLeafIfUnchanged $root 'profile' 'Enable' 'DWord' 1
  Restore-LegacyRegistryLeafIfUnchanged $root 'profile' 'Enable' 'DWord' 1
  $key=$root.OpenSubKey('profile',$true);$key.SetValue('Other',1,[Microsoft.Win32.RegistryValueKind]::DWord);$key.Dispose()
  $reject=$false;try{Restore-LegacyRegistryLeafIfUnchanged $root 'profile' 'Enable' 'DWord' 1}catch{$reject=$true};if(-not $reject){throw 'unexpected data overwritten'}
  $key=$root.OpenSubKey('profile',$true);$key.DeleteValue('Other');$key.Dispose();Remove-LegacyRegistryLeafIfUnchanged $root 'profile' 'Enable' 'DWord' 1
  $bytes=[byte[]](1,2,3);$key=$root.CreateSubKey('marker');$key.SetValue('Record',$bytes,[Microsoft.Win32.RegistryValueKind]::Binary);$key.Dispose()
  $child=$root.OpenSubKey('marker',$true);$nested=$child.CreateSubKey('other');$nested.Dispose();$child.Dispose()
  $reject=$false;try{Remove-LegacyRegistryLeafIfUnchanged $root 'marker' 'Record' 'Binary' $bytes}catch{$reject=$true};if(-not $reject){throw 'non-leaf deleted'}
  $child=$root.OpenSubKey('marker',$true);$child.DeleteSubKey('other');$child.Dispose()
  $reject=$false;try{Remove-LegacyRegistryLeafIfUnchanged $root 'marker' 'Record' 'Binary' ([byte[]](9,9))}catch{$reject=$true};if(-not $reject){throw 'different marker deleted'}
  Remove-LegacyRegistryLeafIfUnchanged $root 'marker' 'Record' 'Binary' $bytes
  Restore-LegacyRegistryLeafIfUnchanged $root 'marker' 'Record' 'Binary' $bytes
  Remove-LegacyRegistryLeafIfUnchanged $root 'marker' 'Record' 'Binary' $bytes
  'legacy_recovery_registry_tests_passed=8'
} finally { $root.Dispose();$software.DeleteSubKey($testName,$false);$software.Dispose() }
