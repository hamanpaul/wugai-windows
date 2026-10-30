$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\wix\legacy-recovery-backup.ps1')
$root=Join-Path $env:TEMP ('liukai-backup-test-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root)|Out-Null
$receipt=New-LegacyRecoveryBackup -Root $root -Snapshot ([pscustomobject]@{sid='test';marker='aGVsbG8=';enabled=1}) -Fingerprint ('a'*64)
if(-not (Test-LegacyRecoveryBackup $receipt)){throw 'Valid backup rejected'}
$content=Get-Content -LiteralPath $receipt.Path -Raw -Encoding UTF8|ConvertFrom-Json
if($content.snapshot.sid -cne 'test' -or $content.snapshot.enabled -ne 1){throw 'Snapshot content differs'}
[IO.File]::AppendAllText($receipt.Path,'corruption')
if(Test-LegacyRecoveryBackup $receipt){throw 'Corrupt backup accepted'}
'legacy_recovery_backup_tests_passed=3'
