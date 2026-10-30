$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-transaction.ps1')
# Fake policy for testing coordinator sequencing only; production uses the
# independently tested strict policy and a real state adapter.
function Assert-LegacyProfileRecoveryPreconditions($State) { if(-not $State.Valid){throw 'invalid prestate'} }
function New-FakeStore {
  $script:stage='both';$script:backup=$false;$script:completed=$false;$script:restored=$false;$script:calls=0;$script:mode='ok'
  return @{
    ReadState = { $script:calls++; [pscustomobject]@{Valid=$true;Version=if($script:mode -eq 'drift' -and $script:calls -gt 1){2}else{1}} }
    Fingerprint = { param($state) if($state.Version -eq 1){'a'*64}else{'b'*64} }
    SaveDurableBackup = { param($state,$fp) $script:backup=$true; 'receipt' }
    VerifyBackup = { param($receipt,$fp) $script:mode -ne 'backupfail' }
    RemoveProfileIfUnchanged = { param($state) $script:stage='marker' }
    RemoveMarkerIfUnchanged = { param($state) if($script:mode -in @('removefail','restorefail')){throw 'marker removal failed'}; $script:stage='none' }
    VerifyAbsent = { param($state) $script:stage -eq 'none' -and $script:mode -ne 'verifyfail' }
    MarkComplete = { param($r) $script:completed=$true }
    RestoreIfUnchanged = { param($r) if($script:mode -eq 'restorefail'){throw 'restore failed'};$script:stage='both' }
    VerifyRestored = { param($r) $script:stage -eq 'both' }
    MarkRestored = { param($r) $script:restored=$true }
  }
}
$store=New-FakeStore; $null=Invoke-LegacyRecoveryTransaction $store
if($script:stage -ne 'none' -or -not $script:backup -or -not $script:completed){throw 'successful recovery wrong'}
foreach($case in @('backupfail','drift','removefail','verifyfail')) {
  $store=New-FakeStore;$script:mode=$case;$failed=$false
  try {$null=Invoke-LegacyRecoveryTransaction $store}catch{$failed=$true}
  if(-not $failed -or $script:stage -ne 'both' -or $script:completed){throw "bad failure behavior: $case"}
  if($case -in @('removefail','verifyfail') -and -not $script:restored){throw "rollback not recorded: $case"}
}
$store=New-FakeStore;$script:mode='restorefail';$message=''
try {$null=Invoke-LegacyRecoveryTransaction $store}catch{$message=$_.Exception.Message}
if($message -notmatch 'restore could not be verified' -or $message -notmatch 'receipt' -or -not $script:backup){throw 'Unrecoverable failure did not preserve and report the backup'}
'legacy_recovery_transaction_tests_passed=6'
$store=New-FakeStore
$store.ReadReceipt={param($r) [pscustomobject]@{State='prepared';Fingerprint='a'*64}}
$store.VerifyRecoveryScope={param($d) $true}
$script:stage='marker'
$result=Resume-LegacyRecoveryTransaction $store 'receipt'
if($result -ne 'restored' -or $script:stage -ne 'both' -or -not $script:restored){throw 'Interrupted operation was not restored'}
$store=New-FakeStore;$store.ReadReceipt={param($r) [pscustomobject]@{State='prepared';Fingerprint='a'*64}};$store.VerifyRecoveryScope={param($d) $false};$script:stage='marker';$failed=$false
try {$null=Resume-LegacyRecoveryTransaction $store 'receipt'}catch{$failed=$true}
if(-not $failed -or $script:stage -ne 'marker'){throw 'Unverified recovery scope accepted'}
'legacy_recovery_resume_tests_passed=2'
