# Verify a newly authored MSI root without adopting any pre-existing or foreign
# files. All paths are inspected without following reparse points. Callers must
# have checked that the root was absent before Windows Installer created it.
function Assert-PreparedMsiRoot {
    param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Version,
          [Parameter(Mandatory=$true)][string]$ManifestSha256,[Parameter(Mandatory=$true)][hashtable]$ExtraHashes)
    $rootPath=[IO.Path]::GetFullPath($Root).TrimEnd('\')
    if($Version -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or $Version.Contains('..') -or $ManifestSha256 -cnotmatch '^[0-9a-f]{64}$'){throw 'Invalid prepared MSI identity.'}
    $rootItem=Get-Item -LiteralPath $rootPath -Force -ErrorAction Stop
    if(-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Prepared MSI root is not a plain directory.'}
    $manifestRelative="versions/$Version/package-manifest.json";$manifest=Join-Path $rootPath $manifestRelative.Replace('/','\')
    foreach($dir in @((Join-Path $rootPath 'versions'),(Join-Path (Join-Path $rootPath 'versions') $Version))){
        $part=Get-Item -LiteralPath $dir -Force -ErrorAction Stop
        if(-not $part.PSIsContainer -or ($part.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Prepared MSI manifest path contains a reparse point.'}
    }
    $manifestItem=Get-Item -LiteralPath $manifest -Force -ErrorAction Stop
    if($manifestItem.PSIsContainer -or ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ManifestSha256){throw 'Prepared MSI manifest mismatch.'}
    $document=Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json
    if($document.format -cne 'liukai-windows-package-v1' -or $document.version -cne $Version -or @($document.architectures).Count -ne 1 -or $document.architectures[0] -cne 'x64'){throw 'Prepared MSI manifest identity mismatch.'}
    $expected=@{};$expected[$manifestRelative]=$ManifestSha256
    foreach($row in @($document.files)){
        $relative=[string]$row.path;$hash=[string]$row.sha256
        if($relative -cnotmatch '^[A-Za-z0-9._/-]+$' -or $relative.Contains('..') -or $relative.StartsWith('/') -or $hash -cnotmatch '^[0-9a-f]{64}$' -or $expected.ContainsKey($relative)){throw 'Invalid prepared MSI manifest entry.'}
        $expected[$relative]=$hash
    }
    foreach($relative in $ExtraHashes.Keys){
        if($relative -cnotmatch '^[A-Za-z0-9._/-]+$' -or $relative.Contains('..') -or $relative.StartsWith('/') -or [string]$ExtraHashes[$relative] -cnotmatch '^[0-9a-f]{64}$' -or $expected.ContainsKey($relative)){throw 'Invalid MSI support identity.'}
        $expected[$relative]=[string]$ExtraHashes[$relative]
    }
    $expectedDirs=@{}
    foreach($relative in $expected.Keys){$parts=$relative.Split('/');$prefix='';for($i=0;$i -lt $parts.Length-1;$i++){$prefix=if($prefix){$prefix+'/'+$parts[$i]}else{$parts[$i]};$expectedDirs[$prefix]=$true}}
    $seen=@{};$pending=New-Object 'System.Collections.Generic.Stack[string]';$pending.Push($rootPath)
    while($pending.Count){$directory=$pending.Pop();foreach($item in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Prepared MSI contains a reparse point.'}
        if($item.PSIsContainer){$relativeDir=$item.FullName.Substring($rootPath.Length+1).Replace('\','/');if(-not $expectedDirs.ContainsKey($relativeDir)){throw "Unexpected MSI directory: $relativeDir"};$pending.Push($item.FullName);continue}
        $relative=$item.FullName.Substring($rootPath.Length+1).Replace('\','/')
        if(-not $expected.ContainsKey($relative) -or $seen.ContainsKey($relative) -or (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected[$relative]){throw "Unexpected or changed MSI file: $relative"}
        $seen[$relative]=$true
    }}
    if($seen.Count -ne $expected.Count){throw 'Prepared MSI file set is incomplete.'}
    return $true
}

# Decode the build-pinned hashes passed by the MSI custom action. Requiring the
# complete fixed set prevents a caller from omitting a support file from proof.
function ConvertFrom-PreparedMsiExtraHashes {
    param([Parameter(Mandatory=$true)][string]$Value)
    $names=@('LiuKaiRegister.exe','support/recover-legacy-profile.ps1','support/legacy-prepared-root.ps1','support/legacy-recovery-policy.ps1','support/legacy-recovery-observe.ps1','support/legacy-recovery-transaction.ps1','support/legacy-recovery-adapter.ps1','support/legacy-recovery-backup.ps1','support/legacy-recovery-registry.ps1','support/legacy-owner-marker.ps1','support/legacy-history-probe.exe','support/collect-install-diagnostics.ps1')
    $hashes=@{};foreach($row in $Value.Split(';')){$parts=$row.Split('=');if($parts.Count -ne 2 -or $parts[0] -cnotin $names -or $parts[1] -cnotmatch '^[0-9a-f]{64}$' -or $hashes.ContainsKey($parts[0])){throw 'Invalid MSI support proof.'};$hashes[$parts[0]]=$parts[1]}
    if($hashes.Count -ne $names.Count){throw 'MSI support proof is incomplete.'}
    return $hashes
}