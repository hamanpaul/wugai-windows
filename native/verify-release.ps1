param([Parameter(Mandatory=$true)][string]$ReleaseRoot,
      [Parameter(Mandatory=$true)][string]$SetupName)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $ReleaseRoot).Path
if($SetupName -notmatch '^WuGai-[A-Za-z0-9._-]+-x64-Setup\.exe$'){throw 'Invalid setup name.'}
$bundle=Join-Path $root 'bundle'; $setup=Join-Path $root $SetupName; $payload=Join-Path $root 'payload.zip'
$manifest=Get-Content -LiteralPath (Join-Path $bundle 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if($manifest.format -cne 'liukai-release-v1' -or $manifest.architecture -cne 'x64'){throw 'Invalid release manifest.'}
$paths=@{}
foreach($row in $manifest.files){
    if($row.path -notmatch '^[A-Za-z0-9._/-]+$' -or $row.path.Contains('..') -or $paths.ContainsKey($row.path)){throw 'Unsafe or duplicate manifest path.'}
    $paths[$row.path]=$true; $file=Join-Path $bundle $row.path.Replace('/','\')
    if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $row.sha256 -or (Get-Item -LiteralPath $file).Length -ne $row.bytes){throw "Bundle mismatch: $($row.path)"}
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($payload)
try {
    if($zip.Entries.Count -ne ($manifest.files.Count+1)){throw 'Unexpected ZIP entry count.'}
    foreach($entry in $zip.Entries){
        $relative=$entry.FullName.Replace('\','/')
        if($relative -cne 'release-manifest.json' -and -not $paths.ContainsKey($relative)){throw 'Unexpected ZIP entry.'}
        $file=Join-Path $bundle $relative.Replace('/','\')
        $stream=$entry.Open();$hash=[Security.Cryptography.SHA256]::Create()
        try {$actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','')} finally {$hash.Dispose();$stream.Dispose()}
        if($actual -cne (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash){throw "ZIP mismatch: $relative"}
    }
} finally {$zip.Dispose()}
$assembly=[Reflection.Assembly]::ReflectionOnlyLoadFrom($setup)
foreach($resource in @('payload.zip','diagnostics.ps1')) {
    $stream=$assembly.GetManifestResourceStream($resource); if(-not $stream){throw "Missing resource: $resource"}
    $hash=[Security.Cryptography.SHA256]::Create()
    try {$actual=[BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','')} finally {$hash.Dispose();$stream.Dispose()}
    $expected=if($resource -eq 'payload.zip'){$payload}else{Join-Path $bundle 'collect-install-diagnostics.ps1'}
    if($actual -cne (Get-FileHash -LiteralPath $expected -Algorithm SHA256).Hash){throw "Embedded resource mismatch: $resource"}
}
$setupHash=(Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedLine="$setupHash  $SetupName"
if((Get-Content -LiteralPath (Join-Path $root 'SHA256SUMS.txt') -Raw).Trim() -cne $expectedLine){throw 'Setup checksum file mismatch.'}
[pscustomobject]@{version=$manifest.version;setup=$setup;sha256=$setupHash;payload_files=$manifest.files.Count;verified=$true}|ConvertTo-Json
