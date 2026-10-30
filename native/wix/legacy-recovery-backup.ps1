# Filesystem portion of the legacy recovery receipt. The caller is responsible
# for placing Root in a trusted, non-reparse user-owned directory and holding
# lifecycle locks. This function never touches the registry.
function New-LegacyRecoveryBackup {
    param([Parameter(Mandatory=$true)][string]$Root,
          [Parameter(Mandatory=$true)]$Snapshot,
          [Parameter(Mandatory=$true)][string]$Fingerprint)
    if($Fingerprint -cnotmatch '^[0-9a-f]{64}$'){throw 'Invalid snapshot fingerprint.'}
    $rootPath=[IO.Path]::GetFullPath($Root)
    $rootItem=Get-Item -LiteralPath $rootPath -Force -ErrorAction Stop
    if(-not $rootItem.PSIsContainer){throw 'Backup root must be an existing normal directory.'}
    $cursor=$rootItem
    while($null -ne $cursor) {
        if($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Backup path contains a reparse point.'}
        $parent=[IO.Directory]::GetParent($cursor.FullName)
        if($null -eq $parent){break}
        $cursor=Get-Item -LiteralPath $parent.FullName -Force -ErrorAction Stop
    }
    $job=[Guid]::NewGuid().ToString('N')
    $record=[ordered]@{format='liukai-legacy-recovery-v1';state='prepared';fingerprint=$Fingerprint;created_utc=[DateTime]::UtcNow.ToString('o');snapshot=$Snapshot}
    $utf8=New-Object Text.UTF8Encoding($false)
    $bytes=$utf8.GetBytes(($record|ConvertTo-Json -Depth 20 -Compress))
    $hash=[Security.Cryptography.SHA256]::Create()
    try {$digest=[BitConverter]::ToString($hash.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()} finally {$hash.Dispose()}
    $path=Join-Path $rootPath ($job+'-'+$digest+'.json')
    $stream=New-Object IO.FileStream($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try { $stream.Write($bytes,0,$bytes.Length);$stream.Flush($true) } finally {$stream.Dispose()}
    $actual=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if($actual -cne $digest){throw "Backup readback mismatch; preserve $path"}
    return [pscustomobject]@{Path=$path;Sha256=$digest;Fingerprint=$Fingerprint}
}
function Test-LegacyRecoveryBackup {
    param([Parameter(Mandatory=$true)]$Receipt)
    if($Receipt.Sha256 -cnotmatch '^[0-9a-f]{64}$' -or $Receipt.Fingerprint -cnotmatch '^[0-9a-f]{64}$'){return $false}
    try {
        $item=Get-Item -LiteralPath $Receipt.Path -Force -ErrorAction Stop
        if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){return $false}
        if((Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Receipt.Sha256){return $false}
        $record=Get-Content -LiteralPath $item.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        return $record.format -ceq 'liukai-legacy-recovery-v1' -and $record.state -ceq 'prepared' -and $record.fingerprint -ceq $Receipt.Fingerprint
    } catch { return $false }
}
