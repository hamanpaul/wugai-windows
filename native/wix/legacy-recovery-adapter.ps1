# Concrete storage adapter for the recovery coordinator. The application that
# constructs it must first validate paths and acquire BOTH product locks.
# The observer and postcheck must execute in the initiating user's environment.
. (Join-Path $PSScriptRoot 'legacy-recovery-backup.ps1')
. (Join-Path $PSScriptRoot 'legacy-recovery-registry.ps1')
function Get-LegacyStateFingerprint($State) {
    $bytes=(New-Object Text.UTF8Encoding($false)).GetBytes(($State|ConvertTo-Json -Depth 20 -Compress))
    $sha=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
}
function Write-LegacyRecoveryOutcome($Receipt,[string]$Outcome) {
    if($Outcome -cnotin @('complete','restored')){throw 'Invalid outcome.'}
    if(-not (Test-LegacyRecoveryBackup $Receipt)){throw 'Backup verification failed before outcome.'}
    $path=$Receipt.Path+'.'+$Outcome
    $data=(New-Object Text.UTF8Encoding($false)).GetBytes($Receipt.Sha256+"`n")
    $stream=New-Object IO.FileStream($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try {$stream.Write($data,0,$data.Length);$stream.Flush($true)} finally {$stream.Dispose()}
    if(([IO.File]::ReadAllText($path)).Trim() -cne $Receipt.Sha256){throw 'Outcome readback failed.'}
}
function New-LegacyRecoveryStore {
    param([Parameter(Mandatory=$true)]$ProfileParent,[Parameter(Mandatory=$true)]$MarkerParent,
          [Parameter(Mandatory=$true)][string]$BackupRoot,[Parameter(Mandatory=$true)][scriptblock]$Observer,
          [Parameter(Mandatory=$true)][scriptblock]$Postcheck,
          [scriptblock]$ResumeScope={ $false })
    $ctx=[pscustomobject]@{Profile=$ProfileParent;Marker=$MarkerParent;BackupRoot=$BackupRoot;Observer=$Observer;Postcheck=$Postcheck;ResumeScope=$ResumeScope}
    $profileLeaf='{7254A21F-6EB1-4217-A42C-C27301175D90}'
    $markerLeaf='Profile'
    return @{
        ReadState = { & $ctx.Observer }.GetNewClosure()
        Fingerprint = { param($s) Get-LegacyStateFingerprint $s }
        SaveDurableBackup = { param($s,$fingerprint) New-LegacyRecoveryBackup -Root $ctx.BackupRoot -Snapshot $s -Fingerprint $fingerprint }.GetNewClosure()
        VerifyBackup = { param($r,$fp) (Test-LegacyRecoveryBackup $r) -and $r.Fingerprint -ceq $fp }
        RemoveProfileIfUnchanged = {param($s) Remove-LegacyRegistryLeafIfUnchanged $ctx.Profile $profileLeaf 'Enable' 'DWord' ([int]$s.ProfileEnable)}.GetNewClosure()
        RemoveMarkerIfUnchanged = {param($s) Remove-LegacyRegistryLeafIfUnchanged $ctx.Marker $markerLeaf 'Record' 'Binary' ([Convert]::FromBase64String($s.MarkerBytes))}.GetNewClosure()
        VerifyAbsent = {param($s) (Test-LegacyRegistryLeaf $ctx.Profile $profileLeaf 'Enable' 'DWord' ([int]$s.ProfileEnable)) -ceq 'absent' -and (Test-LegacyRegistryLeaf $ctx.Marker $markerLeaf 'Record' 'Binary' ([Convert]::FromBase64String($s.MarkerBytes))) -ceq 'absent' -and (& $ctx.Postcheck)}.GetNewClosure()
        MarkComplete = {param($r) Write-LegacyRecoveryOutcome $r 'complete'}
        RestoreIfUnchanged = {param($r)
            if(-not (Test-LegacyRecoveryBackup $r)){throw 'Cannot restore an unverified backup.'}
            $snapshot=(Get-Content -LiteralPath $r.Path -Raw -Encoding UTF8|ConvertFrom-Json).snapshot
            Restore-LegacyRegistryLeafIfUnchanged $ctx.Profile $profileLeaf 'Enable' 'DWord' ([int]$snapshot.ProfileEnable)
            Restore-LegacyRegistryLeafIfUnchanged $ctx.Marker $markerLeaf 'Record' 'Binary' ([Convert]::FromBase64String($snapshot.MarkerBytes))
        }.GetNewClosure()
        VerifyRestored = {param($r)
            if(-not (Test-LegacyRecoveryBackup $r)){return $false}
            $snapshot=(Get-Content -LiteralPath $r.Path -Raw -Encoding UTF8|ConvertFrom-Json).snapshot
            (Test-LegacyRegistryLeaf $ctx.Profile $profileLeaf 'Enable' 'DWord' ([int]$snapshot.ProfileEnable)) -ceq 'matches' -and (Test-LegacyRegistryLeaf $ctx.Marker $markerLeaf 'Record' 'Binary' ([Convert]::FromBase64String($snapshot.MarkerBytes))) -ceq 'matches' -and ((Get-LegacyStateFingerprint (& $ctx.Observer)) -ceq $r.Fingerprint)
        }.GetNewClosure()
        MarkRestored = {param($r) Write-LegacyRecoveryOutcome $r 'restored'}
        ReadReceipt = {param($r)
            if(-not (Test-LegacyRecoveryBackup $r)){throw 'Unverified receipt.'}
            $record=Get-Content -LiteralPath $r.Path -Raw -Encoding UTF8 | ConvertFrom-Json
            Assert-LegacyProfileRecoveryPreconditions $record.snapshot
            if((Get-LegacyStateFingerprint $record.snapshot) -cne $r.Fingerprint){throw 'Recovery snapshot fingerprint mismatch.'}
            $state='prepared';$done=Test-Path -LiteralPath ($r.Path+'.complete');$restored=Test-Path -LiteralPath ($r.Path+'.restored')
            if($done -and $restored){throw 'Conflicting recovery outcome files.'}
            foreach($suffix in @('complete','restored')) {
                $path=$r.Path+'.'+$suffix
                if(Test-Path -LiteralPath $path) {
                    $item=Get-Item -LiteralPath $path -Force
                    if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                       ([IO.File]::ReadAllText($path)).Trim() -cne $r.Sha256){throw 'Invalid recovery outcome file.'}
                    $state=$suffix
                }
            }
            [pscustomobject]@{State=$state;Fingerprint=$r.Fingerprint;Snapshot=$record.snapshot}
        }
        VerifyRecoveryScope = {param($document) & $ctx.ResumeScope $document}.GetNewClosure()
    }
}

# Enumerate a dedicated, trusted receipt folder. A prepared record indicates a
# possible interruption and must be resolved before a new mutation begins.
function Get-LegacyRecoveryPendingReceipts {
    param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)]$Store)
    $files=@(Get-ChildItem -LiteralPath $Root -Force)
    foreach($file in $files){if($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unexpected recovery evidence type.'}}
    $known=@{}
    foreach($file in $files) {
        if($file.Name -cnotmatch '^([0-9a-f]{32})-([0-9a-f]{64})\.json$'){continue}
        $sha=$Matches[2]
        $record=Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $receipt=[pscustomobject]@{Path=$file.FullName;Sha256=$sha;Fingerprint=$record.fingerprint}
        $document=& $Store.ReadReceipt $receipt
        $known[$file.Name]=$true
        if(Test-Path -LiteralPath ($file.FullName+'.complete')){$known[$file.Name+'.complete']=$true}
        if(Test-Path -LiteralPath ($file.FullName+'.restored')){$known[$file.Name+'.restored']=$true}
        if($document.State -ceq 'prepared'){ $receipt }
    }
    foreach($file in $files) {if(-not $known.ContainsKey($file.Name)){throw "Unexpected recovery evidence: $($file.Name)"}}
}
